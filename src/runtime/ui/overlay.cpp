#include "runtime/ui/overlay.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <d3d11_1.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "common/settings_file.h"
#include "runtime/bindings.h"
#include "runtime/config.h"
#include "runtime/gamepads.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/settings.h"
#include "runtime/sysdialog.h"

namespace rt::ui {
namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Model: two pages of rows, rebuilt from the live settings after every change.
enum class Kind { Choice, Slider, Binding, Buttons };
enum Id { kFps, kMode, kMonitor, kWindow, kFsResolution, kFsRefresh, kRenderScale, kMouseMode, kSensitivityX, kSensitivityY, kInvertX, kInvertY, kBind, kButtons };

struct Row {
  Kind kind = Kind::Choice;
  Id id = kFps;
  std::string label, tag, value_note, side_note;  // Slider: notes left and right of the bar
  std::string help;                               // shown in the help bar while the row has the focus
  std::vector<std::string> options;  // Choice
  int index = 0;
  bool enabled = true;
  double value = 0, lo = 0, hi = 1, step = 0.1;  // Slider
  size_t control = 0;                            // Binding
  bool menu = false;                             // Binding: the [menu] layer (keys in the game's menus)
  std::string slots[2];                          // Binding: input names ("" = none)
};

const char* const kPages[] = {"DISPLAY", "KEYBOARD / MOUSE", "MENU KEYS"};
constexpr int kPageCount = 3;

// Action names of the pad controls under the game's control TYPE A (GAME_NOTES "Controls"), and
// the control.
struct ControlText {
  const char* control;
  const char* action;
  const char* pad;
};
const ControlText kControlText[] = {
    {"lstick_up", "MOVE FORWARD", "L STICK UP"},     {"lstick_down", "MOVE BACK", "L STICK DOWN"},
    {"lstick_left", "MOVE LEFT", "L STICK LEFT"},    {"lstick_right", "MOVE RIGHT", "L STICK RIGHT"},
    {"rstick_up", "LOOK UP", "R STICK UP"},          {"rstick_down", "LOOK DOWN", "R STICK DOWN"},
    {"rstick_left", "TURN LEFT", "R STICK LEFT"},    {"rstick_right", "TURN RIGHT", "R STICK RIGHT"},
    {"cross", "RIGHT WEAPON", "CROSS"},              {"square", "LEFT WEAPON", "SQUARE"},
    {"circle", "SHOULDER WEAPON", "CIRCLE"},         {"r1", "SWITCH RIGHT WEAPON", "R1"},
    {"l1", "SWITCH LEFT WEAPON", "L1"},              {"l2", "BOOST", "L2"},
    {"r2", "QUICK BOOST", "R2"},                     {"triangle", "OVER BOOST", "TRIANGLE"},
    {"r3", "RESET VIEW", "R3"},                      {"l3", "LOCK ON TARGET", "L3"},
    {"start", "PAUSE", "START"},                     {"select", "SELECT BUTTON", "SELECT"},
    {"up", "MENU UP", "D-PAD UP"},                   {"down", "MENU DOWN", "D-PAD DOWN"},
    {"left", "MENU LEFT", "D-PAD LEFT"},             {"right", "MENU RIGHT", "D-PAD RIGHT"},
    {"purge_right_weapon", "PURGE RIGHT WEAPON", "R1 + L1 + CROSS"},
    {"purge_left_weapon", "PURGE LEFT WEAPON", "R1 + L1 + SQUARE"},
    {"purge_shoulder_weapon", "PURGE SHOULDER WEAPON", "R1 + L1 + CIRCLE"},
};
const ControlText* control_text(size_t c) {
  std::string name = bindings::control_name(c);
  for (const auto& t : kControlText)
    if (name == t.control) return &t;
  return nullptr;
}
// The MENU KEYS page: what the controls do in the game's menus.
const ControlText kMenuText[] = {
    {"up", "UP", "D-PAD UP"},           {"down", "DOWN", "D-PAD DOWN"},        {"left", "LEFT", "D-PAD LEFT"},
    {"right", "RIGHT", "D-PAD RIGHT"},  {"cross", "CHOOSE", "CROSS"},          {"circle", "BACK", "CIRCLE"},
    {"start", "START / KEY GUIDE", "START"}, {"l1", "PREVIOUS PAGE", "L1"},    {"r1", "NEXT PAGE", "R1"},
    {"triangle", "TRIANGLE BUTTON", "TRIANGLE"}, {"square", "SQUARE BUTTON", "SQUARE"}, {"l2", "L2 BUTTON", "L2"},
    {"select", "SELECT BUTTON", "SELECT"},
};
const ControlText* menu_text(size_t c) {
  std::string name = bindings::control_name(c);
  for (const auto& t : kMenuText)
    if (name == t.control) return &t;
  return nullptr;
}
const ControlText* text_for(size_t c, bool menu) { return menu ? menu_text(c) : control_text(c); }

std::string upper(std::string s) {
  for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

std::string key_label(u32 code) {
  std::string n = bindings::input_name(code);
  if (n == "Mouse1") return "LEFT CLICK";
  if (n == "Mouse2") return "RIGHT CLICK";
  if (n == "Mouse3") return "MIDDLE CLICK";
  if (n == "Mouse4") return "MOUSE 4";
  if (n == "Mouse5") return "MOUSE 5";
  if (n == "WheelUp") return "WHEEL UP";
  if (n == "WheelDown") return "WHEEL DOWN";
  if (n.rfind("Num", 0) == 0 && n.size() == 4) return "NUMPAD " + n.substr(3);
  return upper(n);
}

// Screen state. The window thread changes it (input, controller polling); the presenting thread
// draws a copy; the pad module reads the controller mode.
enum class State { Closed, Open };
std::recursive_mutex g_mu;
std::atomic<State> g_state{State::Closed};
std::atomic<bool> g_open_request{false};
int g_page = 0, g_focus = 0, g_slot = 0, g_scroll = 0;
bool g_capturing = false;
std::vector<Row> g_rows;
std::string g_toast;
Clock::time_point g_toast_until, g_default_armed_until;
bool g_restore_capture = false;
int g_client_w = 1280, g_client_h = 720;
bool g_drag_slider = false;
std::atomic<u16> g_suppress[4];
u16 g_pad_held = 0;  // controller buttons held during the last poll (window thread)

std::vector<Row> build_rows(int page) {
  std::vector<Row> rows;
  if (page == 0) {
    ac4::settings::Display d = settings::display();
    double fps = cfg().fps.load();
    Row r;
    r.kind = Kind::Choice;
    r.id = kFps;
    r.label = "FRAME RATE";
    r.index = -1;
    for (const auto& c : ac4::settings::fps_choices()) {
      r.options.push_back(upper(c.label));
      if (std::fabs(c.fps - fps) < 0.01) r.index = int(r.options.size()) - 1;
    }
    if (r.index < 0) {
      r.options.push_back(ac4::settings::fps_text(fps) + "  (UNTESTED)");
      r.index = int(r.options.size()) - 1;
    }
    rows.push_back(r);
    using ac4::settings::Mode;
    Row m;
    m.kind = Kind::Choice;
    m.id = kMode;
    m.label = "DISPLAY MODE";
    m.options = {"WINDOWED", "FULLSCREEN", "BORDERLESS FULLSCREEN"};
    m.index = d.mode == Mode::Fullscreen ? 1 : d.mode == Mode::Borderless ? 2 : 0;
    rows.push_back(m);
    std::vector<gpu::Monitor> monitors = gpu::window_monitors();
    if (monitors.size() > 1) {  // which monitor the game uses
      Row mon;
      mon.kind = Kind::Choice;
      mon.id = kMonitor;
      mon.label = "MONITOR";
      mon.index = 0;
      for (size_t i = 0; i < monitors.size(); ++i) {
        const gpu::Monitor& g = monitors[i];
        mon.options.push_back(ac4::strprintf("%zu   %d x %d%s", i + 1, g.w, g.h, g.primary ? "   (PRIMARY)" : ""));
        if (d.monitor.empty() ? g.primary : g.device == d.monitor) mon.index = int(i);
      }
      rows.push_back(mon);
    }
    Row w;
    w.kind = Kind::Choice;
    w.label = "RESOLUTION";
    w.index = -1;
    int mw = 0, mh = 0;
    gpu::window_monitor_size(&mw, &mh);
    if (d.mode == Mode::Windowed) {  // the window's size
      w.id = kWindow;
      int aw = 0, ah = 0;
      gpu::window_work_area(&aw, &ah);
      for (const auto& s : ac4::settings::window_sizes()) {
        if (aw && (s.w > aw || s.h > ah) && !(s.w == d.width && s.h == d.height)) continue;
        w.options.push_back(ac4::strprintf("%d x %d", s.w, s.h));
        if (s.w == d.width && s.h == d.height) w.index = int(w.options.size()) - 1;
      }
      if (w.index < 0) {
        w.options.push_back(ac4::strprintf("%d x %d", d.width, d.height));
        w.index = int(w.options.size()) - 1;
      }
      rows.push_back(w);
    } else if (d.mode == Mode::Borderless) {  // the monitor as it is
      w.id = kWindow;
      w.enabled = false;
      w.options.push_back(ac4::strprintf("%d x %d", mw, mh));
      w.index = 0;
      rows.push_back(w);
    } else {  // exclusive fullscreen: the output's display modes
      w.id = kFsResolution;
      std::vector<gpu::display::Mode> modes = gpu::display::modes();
      int cw = d.fs_width ? d.fs_width : mw, ch = d.fs_width ? d.fs_height : mh;
      for (const auto& md : modes) {
        std::string t = ac4::strprintf("%u x %u", md.w, md.h);
        if (std::find(w.options.begin(), w.options.end(), t) == w.options.end()) w.options.push_back(t);
        if (int(md.w) == cw && int(md.h) == ch) w.index = int(std::find(w.options.begin(), w.options.end(), t) - w.options.begin());
      }
      if (w.index < 0) {
        w.options.push_back(ac4::strprintf("%d x %d", cw, ch));
        w.index = int(w.options.size()) - 1;
      }
      rows.push_back(w);
      Row hz;
      hz.kind = Kind::Choice;
      hz.id = kFsRefresh;
      hz.label = "REFRESH RATE";
      hz.index = -1;
      int want = d.fs_hz ? d.fs_hz : int(gpu::display::refresh_hz());
      for (auto it = modes.rbegin(); it != modes.rend(); ++it)  // ascending
        if (int(it->w) == cw && int(it->h) == ch) {
          std::string t = ac4::strprintf("%u HZ", it->hz);
          if (std::find(hz.options.begin(), hz.options.end(), t) != hz.options.end()) continue;
          hz.options.push_back(t);
          if (int(it->hz) == want) hz.index = int(hz.options.size()) - 1;
        }
      if (hz.index < 0) {
        hz.options.push_back(ac4::strprintf("%d HZ", want));
        hz.index = int(hz.options.size()) - 1;
      }
      rows.push_back(hz);
    }
    // The resolution the game renders at: the 16:9 picture in the window or on the screen, times this.
    Row rs;
    rs.kind = Kind::Slider;
    rs.id = kRenderScale;
    rs.label = "RENDER SCALE";
    rs.value = d.render_scale;
    rs.lo = ac4::settings::kRenderScaleMin;
    rs.hi = ac4::settings::kRenderScaleMax;
    rs.step = 5;
    {
      int cw = 0, ch = 0;
      gpu::window_client_size(&cw, &ch);
      double fit_h = std::min(double(ch), double(cw) * 9.0 / 16.0) * d.render_scale / 100.0;
      long rh = std::lround(std::clamp(fit_h, 360.0, 2880.0)), rw = std::lround(double(rh) * 16.0 / 9.0);
      rs.value_note = ac4::strprintf("%d %%", d.render_scale);
      rs.side_note = ac4::strprintf("%ld x %ld", rw, rh);
    }
    rows.push_back(rs);
  } else {
    bindings::MouseSettings ms = bindings::mouse();
    if (page == 1) {  // the mouse as the right stick (the default) or moving the camera
      Row mm;
      mm.kind = Kind::Choice;
      mm.id = kMouseMode;
      mm.label = "MOUSE MODE";
      mm.options = {"STICK", "CAMERA"};
      mm.index = ms.mode == bindings::MouseMode::Camera ? 1 : 0;
      rows.push_back(mm);
    }
    for (int axis = 0; axis < 2 && page == 1; ++axis) {
      Row s;
      s.kind = Kind::Slider;
      s.id = axis ? kSensitivityY : kSensitivityX;
      s.label = axis ? "MOUSE SENSITIVITY Y" : "MOUSE SENSITIVITY X";
      s.value = axis ? ms.sensitivity_y : ms.sensitivity_x;
      s.lo = 0.1;
      s.hi = 5.0;
      s.step = 0.05;
      s.value_note = ac4::strprintf("%.2f", s.value);
      rows.push_back(s);
    }
    for (int axis = 0; axis < 2 && page == 1; ++axis) {
      Row inv;
      inv.kind = Kind::Choice;
      inv.id = axis ? kInvertY : kInvertX;
      inv.label = axis ? "INVERT MOUSE Y" : "INVERT MOUSE X";
      inv.options = {"OFF", "ON"};
      inv.index = (axis ? ms.invert_y : ms.invert_x) ? 1 : 0;
      rows.push_back(inv);
    }
    const bool menu = page == 2;
    std::vector<size_t> order;  // the MENU KEYS page in its own order (directions, choose, back, ...)
    if (menu) {
      for (const ControlText& t : kMenuText)
        for (size_t c = 0; c < bindings::control_count(); ++c)
          if (t.control == std::string(bindings::control_name(c))) order.push_back(c);
    } else {
      for (size_t c = 0; c < bindings::control_count(); ++c) order.push_back(c);
    }
    for (size_t c : order) {
      const ControlText* t = text_for(c, menu);
      Row b;
      b.kind = Kind::Binding;
      b.id = kBind;
      b.control = c;
      b.menu = menu;
      b.label = t ? t->action : upper(bindings::control_name(c));
      b.tag = t ? t->pad : "";
      // In missions the keys press the action's button of the game's control type (bindings.h).
      if (!menu) {
        std::string live = bindings::mission_button(c);
        if (!live.empty()) b.tag = live;
      }
      std::vector<u32> codes = bindings::get(c, menu ? bindings::Layer::Menu : bindings::Layer::Game);
      for (size_t i = 0; i < 2 && i < codes.size(); ++i) b.slots[i] = key_label(codes[i]);
      if (codes.size() > 2) b.slots[1] += ac4::strprintf("  +%zu", codes.size() - 2);
      rows.push_back(b);
    }
  }
  Row btn;
  btn.kind = Kind::Buttons;
  btn.id = kButtons;
  rows.push_back(btn);
  return rows;
}

// ---------------------------------------------------------------------------------------------
// Layout: a 1280x720 canvas scaled uniformly to the window (as the game's own screens are).
constexpr float kW = 1280, kH = 720;
constexpr float kListTop = 128, kRowH = 37, kPitch = 46, kListBottom = 580;
constexpr float kRowL = 76, kRowR = 1204;
constexpr float kButtonsY = 590;
constexpr int kVisibleRows = int((kListBottom - kListTop + kPitch - kRowH) / kPitch);  // 10
constexpr float kSlotX[2] = {600, 905}, kSlotW = 290, kBindDivider = 466;  // binding rows: wider caption column
constexpr float kArrowL = 472, kArrowR = 1054, kBarL = 505, kBarR = 1023;
constexpr float kTabY = 34, kTabH = 30;
constexpr float kTabX[kPageCount] = {656, 816, 1064}, kTabW[kPageCount] = {150, 238, 140};
constexpr float kButtonX[2] = {76, 287}, kButtonW = 199;

struct Canvas {
  float scale = 1, ox = 0, oy = 0;
};
Canvas canvas_for(int w, int h) {
  Canvas c;
  c.scale = std::min(float(w) / kW, float(h) / kH);
  c.ox = std::floor((float(w) - kW * c.scale) / 2);
  c.oy = std::floor((float(h) - kH * c.scale) / 2);
  return c;
}

// System dialogs on the same canvas: a framed panel over the dimmed game, its heading and rule,
// then the list (rows, the highlighted row's details), the question's or message's text, or the
// text entry (prompt, typed text, on-screen keyboard, help); buttons at the bottom right.
struct Box {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool has(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
struct DialogLayout {
  Box panel, heading, text, edit, detail, help;
  float rule_y = 0;
  std::vector<Box> rows;  // the rows shown: row scroll + i
  std::vector<Box> keys;
  Box buttons[2];
  int buttons_n = 0;
};
constexpr float kDlgRowPitch = 44, kDlgRowH = 37, kDlgKeyPitch = 46, kDlgKeyH = 40, kDlgButtonW = 160;
DialogLayout dialog_layout(const DialogView& v) {
  DialogLayout L;
  bool tall = v.kind == DialogView::List || v.kind == DialogView::Text;
  L.panel = tall ? Box{190, 96, 1090, 664} : Box{290, 220, 990, 500};
  float x0 = L.panel.x0 + 36, x1 = L.panel.x1 - 36;
  L.heading = {x0, L.panel.y0 + 16, x1, L.panel.y0 + 56};
  L.rule_y = L.panel.y0 + 64;
  float by0 = L.panel.y1 - 58, by1 = by0 + kDlgRowH;
  if (v.kind == DialogView::Message) {
    L.buttons_n = 1;
    L.buttons[0] = {x1 - kDlgButtonW, by0, x1, by1};
  } else {
    L.buttons_n = 2;
    L.buttons[0] = {x1 - 2 * kDlgButtonW - 10, by0, x1 - kDlgButtonW - 10, by1};
    L.buttons[1] = {x1 - kDlgButtonW, by0, x1, by1};
  }
  switch (v.kind) {
    case DialogView::List: {
      float top = L.rule_y + 16;
      int n = std::min(kDialogListRows, int(v.rows.size()) - v.scroll);
      for (int i = 0; i < n; ++i) L.rows.push_back({x0, top + float(i) * kDlgRowPitch, x1 - 26, top + float(i) * kDlgRowPitch + kDlgRowH});
      L.detail = {x0 + 14, top + kDialogListRows * kDlgRowPitch + 4, x1, by0 - 8};
      break;
    }
    case DialogView::Question:
    case DialogView::Message: L.text = {x0, L.rule_y + 16, x1, by0 - 10}; break;
    case DialogView::Text: {
      L.text = {x0, L.rule_y + 12, x1, L.rule_y + 76};
      L.edit = {x0, L.rule_y + 84, x1, L.rule_y + 126};
      int n = int(v.keys.size()), cols = kDialogKeyCols, rows = (n + cols - 1) / cols;
      float kw = (x1 - x0) / float(cols), ky = L.rule_y + 140;
      for (int i = 0; i < n; ++i) {
        float kx = x0 + float(i % cols) * kw, y = ky + float(i / cols) * kDlgKeyPitch;
        L.keys.push_back({kx + 2, y, kx + kw - 2, y + kDlgKeyH});
      }
      L.help = {x0, ky + float(rows) * kDlgKeyPitch + 6, x1, by0 - 6};
      break;
    }
  }
  return L;
}

int list_rows() { return int(g_rows.size()) - 1; }  // without the buttons row

void clamp_scroll() {
  int n = list_rows();
  if (g_focus < n) {
    if (g_focus < g_scroll) g_scroll = g_focus;
    if (g_focus >= g_scroll + kVisibleRows) g_scroll = g_focus - kVisibleRows + 1;
  }
  g_scroll = std::clamp(g_scroll, 0, std::max(0, n - kVisibleRows));
}

void rebuild() {
  g_rows = build_rows(g_page);
  g_focus = std::clamp(g_focus, 0, int(g_rows.size()) - 1);
  clamp_scroll();
}

void toast(const std::string& s) {
  g_toast = s;
  g_toast_until = Clock::now() + std::chrono::seconds(3);
}

// ---------------------------------------------------------------------------------------------
// Actions (window thread, g_mu held).
void apply_choice(Row& r, int index) {
  if (!r.enabled || r.options.empty()) return;
  index = std::clamp(index, 0, int(r.options.size()) - 1);
  if (index == r.index) return;
  switch (r.id) {
    case kFps: {
      const auto& choices = ac4::settings::fps_choices();
      if (index < int(choices.size())) settings::set_fps(choices[size_t(index)].fps);
      break;
    }
    case kMode:
      settings::set_mode(index == 1   ? ac4::settings::Mode::Fullscreen
                         : index == 2 ? ac4::settings::Mode::Borderless
                                      : ac4::settings::Mode::Windowed);
      break;
    case kFsResolution: {  // keeps the refresh rate when the new resolution has it, else its highest
      int w = 0, h = 0;
      if (std::sscanf(r.options[size_t(index)].c_str(), "%d x %d", &w, &h) != 2) break;
      int keep = settings::display().fs_hz, best = 0;
      bool has_keep = false;
      for (const auto& md : gpu::display::modes())
        if (int(md.w) == w && int(md.h) == h) {
          best = std::max(best, int(md.hz));
          has_keep |= int(md.hz) == keep;
        }
      settings::set_fullscreen_mode(w, h, has_keep ? keep : best);
      break;
    }
    case kFsRefresh: {
      ac4::settings::Display d = settings::display();
      int mw = 0, mh = 0, hz = 0;
      gpu::window_monitor_size(&mw, &mh);
      if (std::sscanf(r.options[size_t(index)].c_str(), "%d", &hz) == 1)
        settings::set_fullscreen_mode(d.fs_width ? d.fs_width : mw, d.fs_width ? d.fs_height : mh, hz);
      break;
    }
    case kMonitor: {
      std::vector<gpu::Monitor> monitors = gpu::window_monitors();
      if (size_t(index) < monitors.size()) settings::set_monitor(monitors[size_t(index)].primary ? "" : monitors[size_t(index)].device);
      break;
    }
    case kWindow: {
      int w = 0, h = 0;
      if (std::sscanf(r.options[size_t(index)].c_str(), "%d x %d", &w, &h) == 2) settings::set_window_size(w, h);
      break;
    }
    case kMouseMode: {
      bindings::MouseSettings m = bindings::mouse();
      m.mode = index == 1 ? bindings::MouseMode::Camera : bindings::MouseMode::Stick;
      bindings::set_mouse(m);
      settings::save_bindings();
      break;
    }
    case kInvertX:
    case kInvertY: {
      bindings::MouseSettings m = bindings::mouse();
      (r.id == kInvertX ? m.invert_x : m.invert_y) = index == 1;
      bindings::set_mouse(m);
      settings::save_bindings();
      break;
    }
    default:
      break;
  }
  rebuild();
}

void apply_slider(Row& r, double v) {
  v = std::clamp(std::round(v / r.step) * r.step, r.lo, r.hi);
  if (std::fabs(v - r.value) < 1e-9) return;
  if (r.id == kSensitivityX || r.id == kSensitivityY) {
    bindings::MouseSettings m = bindings::mouse();
    (r.id == kSensitivityX ? m.sensitivity_x : m.sensitivity_y) = v;
    bindings::set_mouse(m);
    settings::save_bindings();
  } else if (r.id == kRenderScale) {
    settings::set_render_scale(int(std::lround(v)));
  }
  rebuild();
}

void set_binding(size_t control, int slot, u32 code, bool menu) {
  const bindings::Layer layer = menu ? bindings::Layer::Menu : bindings::Layer::Game;
  std::vector<u32> codes = bindings::get(control, layer);
  if (code) {
    // An input drives one control of its layer: taken from any other control there first.
    for (size_t c = 0; c < bindings::control_count(); ++c) {
      if (c == control) continue;
      std::vector<u32> other = bindings::get(c, layer);
      auto it = std::find(other.begin(), other.end(), code);
      if (it == other.end()) continue;
      other.erase(it);
      bindings::set(c, other, layer);
      const ControlText* t = text_for(c, menu);
      toast(key_label(code) + " MOVED FROM " + (t ? t->action : upper(bindings::control_name(c))));
    }
    codes.erase(std::remove(codes.begin(), codes.end(), code), codes.end());
    if (size_t(slot) < codes.size()) codes[size_t(slot)] = code;
    else codes.push_back(code);
  } else if (size_t(slot) < codes.size()) {
    codes.erase(codes.begin() + slot);
  }
  bindings::set(control, codes, layer);
  settings::save_bindings();
  rebuild();
}

void defaults() {
  auto now = Clock::now();
  if (now >= g_default_armed_until) {
    g_default_armed_until = now + std::chrono::seconds(3);
    toast("PRESS AGAIN TO RESET");
    return;
  }
  g_default_armed_until = {};
  g_toast.clear();
  if (g_page == 0) {
    settings::set_fps(60);
    settings::set_mode(ac4::settings::Mode::Windowed);
    settings::set_fullscreen_mode(0, 0, 0);
    settings::set_window_size(1280, 720);
    settings::set_render_scale(100);
  } else if (g_page == 1) {
    bindings::reset_defaults(bindings::Layer::Game);
    bindings::set_mouse(bindings::MouseSettings{});
    settings::save_bindings();
  } else {
    bindings::reset_defaults(bindings::Layer::Menu);
    settings::save_bindings();
  }
  rebuild();
}

void open_locked() {
  g_state = State::Open;
  g_capturing = false;
  g_drag_slider = false;
  g_toast.clear();
  g_focus = 0;
  g_slot = 0;
  g_scroll = 0;
  rebuild();
  g_restore_capture = gpu::window_mouse_captured();
  if (g_restore_capture) gpu::window_set_mouse_capture(false);  // the cursor works on the screen
  LOG_I("ui", "PC SETTINGS opened");
}

void close_locked() {
  if (g_state != State::Open) return;
  g_capturing = false;
  for (auto& s : g_suppress) s = g_pad_held;  // the closing press stays with the screen
  g_state = State::Closed;  // back to the SYSTEM menu, its PC SETTINGS row still chosen
  if (g_restore_capture) gpu::window_set_mouse_capture(true);
  gpu::window_refresh_cursor();  // the menu's cursor rules again
  LOG_I("ui", "PC SETTINGS closed");
}

void activate() {
  if (g_focus < 0 || g_focus >= int(g_rows.size())) return;
  Row& r = g_rows[size_t(g_focus)];
  switch (r.kind) {
    case Kind::Choice:
      apply_choice(r, r.index + 1 < int(r.options.size()) ? r.index + 1 : 0);
      break;
    case Kind::Slider:
      break;
    case Kind::Binding:
      g_capturing = true;
      break;
    case Kind::Buttons:
      if (g_slot == 0) defaults();
      else close_locked();
      break;
  }
}

void move_focus(int d) {
  int n = int(g_rows.size());
  g_focus = std::clamp(g_focus + d, 0, n - 1);
  if (g_rows[size_t(g_focus)].kind == Kind::Buttons) g_slot = std::clamp(g_slot, 0, 1);
  clamp_scroll();
}

void horizontal(int d) {
  Row& r = g_rows[size_t(g_focus)];
  switch (r.kind) {
    case Kind::Choice:
      apply_choice(r, r.index + d);
      break;
    case Kind::Slider:
      apply_slider(r, r.value + d * r.step);
      break;
    case Kind::Binding:
    case Kind::Buttons:
      g_slot = std::clamp(g_slot + d, 0, 1);
      break;
  }
}

void set_page(int p) {
  p = (p + kPageCount) % kPageCount;
  if (p == g_page) return;
  g_page = p;
  g_focus = 0;
  g_slot = 0;
  g_scroll = 0;
  g_capturing = false;
  rebuild();
}

void clear_slot() {
  Row& r = g_rows[size_t(g_focus)];
  if (r.kind != Kind::Binding) return;
  set_binding(r.control, g_slot, 0, r.menu);
}

void capture_input(u32 code) {
  g_capturing = false;
  Row& r = g_rows[size_t(g_focus)];
  if (r.kind != Kind::Binding) return;
  if (!bindings::bindable(code)) {
    toast(key_label(code) + " CANNOT BE BOUND");
    return;
  }
  set_binding(r.control, g_slot, code, r.menu);
}

// ---------------------------------------------------------------------------------------------
// Hit testing in canvas coordinates.
struct Hit {
  enum What { None, Tab, Row, Slot, ArrowL, ArrowR, Bar, Button } what = None;
  int index = 0, slot = 0;
  float x = 0;
};
Hit hit_test(int px, int py) {
  Canvas c = canvas_for(g_client_w, g_client_h);
  float x = (float(px) - c.ox) / c.scale, y = (float(py) - c.oy) / c.scale;
  Hit h;
  h.x = x;
  for (int t = 0; t < kPageCount; ++t)
    if (x >= kTabX[t] && x < kTabX[t] + kTabW[t] && y >= kTabY && y < kTabY + kTabH) {
      h.what = Hit::Tab;
      h.index = t;
      return h;
    }
  if (y >= kButtonsY && y < kButtonsY + kRowH)
    for (int b = 0; b < 2; ++b)
      if (x >= kButtonX[b] && x < kButtonX[b] + kButtonW) {
        h.what = Hit::Button;
        h.index = int(g_rows.size()) - 1;
        h.slot = b;
        return h;
      }
  if (x < kRowL || x >= kRowR || y < kListTop) return h;
  int vis = int((y - kListTop) / kPitch);
  if (vis >= kVisibleRows || y - kListTop - vis * kPitch >= kRowH) return h;
  int i = g_scroll + vis;
  if (i >= list_rows()) return h;
  h.index = i;
  const Row& r = g_rows[size_t(i)];
  h.what = Hit::Row;
  if (r.kind == Kind::Binding) {
    for (int s = 0; s < 2; ++s)
      if (x >= kSlotX[s] && x < kSlotX[s] + kSlotW) {
        h.what = Hit::Slot;
        h.slot = s;
      }
  } else if (x >= kArrowL - 18 && x < kArrowL + 18) {
    h.what = Hit::ArrowL;
  } else if (x >= kArrowR - 18 && x < kArrowR + 18) {
    h.what = Hit::ArrowR;
  } else if (r.kind == Kind::Slider && x >= kBarL && x < kBarR) {
    h.what = Hit::Bar;
  }
  return h;
}

void update_client_size() {
  int w = 0, h = 0;
  gpu::window_client_size(&w, &h);
  if (w > 0 && h > 0) {
    g_client_w = w;
    g_client_h = h;
  }
}

// ---------------------------------------------------------------------------------------------
// Controllers (navigation while open; every connected one: gamepads.h).
u16 g_pad_prev = 0;
int g_stick_prev = 0;  // bit 0 up, 1 down, 2 left, 3 right (left stick past the threshold)
Clock::time_point g_repeat_at;
int g_repeat_dir = 0;

}  // namespace

// ---------------------------------------------------------------------------------------------
bool is_open() { return g_state.load(std::memory_order_relaxed) != State::Closed; }

bool screen_up() { return is_open() || sysdlg::shown(); }

DialogHit dialog_hit(const DialogView& v, int px, int py) {
  int w = 0, h = 0;
  gpu::window_client_size(&w, &h);
  if (w <= 0 || h <= 0) return {};
  Canvas c = canvas_for(w, h);
  float x = (float(px) - c.ox) / c.scale, y = (float(py) - c.oy) / c.scale;
  DialogLayout L = dialog_layout(v);
  for (size_t i = 0; i < L.rows.size(); ++i)
    if (L.rows[i].has(x, y)) return {DialogHit::Row, v.scroll + int(i)};
  for (int b = 0; b < L.buttons_n; ++b)
    if (L.buttons[b].has(x, y)) return {DialogHit::Button, b};
  for (size_t i = 0; i < L.keys.size(); ++i)
    if (L.keys[i].has(x, y)) return {DialogHit::Key, int(i)};
  return {};
}

void open_from_menu() {
  g_open_request = true;  // opened on the window thread (tick)
}

void close() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  close_locked();
}

PadMode pad_mode() { return g_state.load(std::memory_order_relaxed) == State::Closed ? PadMode::Normal : PadMode::Idle; }

bool on_key(u32 vk, bool down, bool repeat) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (g_state != State::Open) return g_state != State::Closed;
  if (!down) return true;
  if (g_capturing) {
    if (repeat) return true;
    if (vk == VK_ESCAPE) g_capturing = false;
    else if (vk == VK_DELETE || vk == VK_BACK) {
      g_capturing = false;
      clear_slot();
    } else {
      capture_input(vk);
    }
    return true;
  }
  bool shift = GetKeyState(VK_SHIFT) < 0;
  switch (vk) {
    case VK_UP: move_focus(-1); break;
    case VK_DOWN: move_focus(1); break;
    case VK_PRIOR: move_focus(-kVisibleRows); break;
    case VK_NEXT: move_focus(kVisibleRows); break;
    case VK_HOME: move_focus(-1000); break;
    case VK_END: move_focus(1000); break;
    case VK_LEFT: horizontal(-1); break;
    case VK_RIGHT: horizontal(1); break;
    case VK_RETURN:
    case VK_SPACE:
      if (!repeat) activate();
      break;
    case VK_DELETE:
    case VK_BACK: clear_slot(); break;
    case VK_TAB: set_page(g_page + (shift ? -1 : 1)); break;
    case 'Q': set_page(g_page - 1); break;
    case 'E': set_page(g_page + 1); break;
    case VK_ESCAPE:
      if (!repeat) close_locked();
      break;
    default: break;
  }
  return true;
}

void on_mouse_move(int x, int y) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (g_state != State::Open || g_capturing) return;
  update_client_size();
  Hit h = hit_test(x, y);
  if (g_drag_slider) {
    Row& r = g_rows[size_t(g_focus)];
    if (r.kind == Kind::Slider) apply_slider(r, r.lo + (r.hi - r.lo) * std::clamp((h.x - kBarL) / (kBarR - kBarL), 0.0f, 1.0f));
    return;
  }
  if (h.what == Hit::Row || h.what == Hit::Slot || h.what == Hit::ArrowL || h.what == Hit::ArrowR || h.what == Hit::Bar ||
      h.what == Hit::Button) {
    g_focus = h.index;
    if (h.what == Hit::Slot || h.what == Hit::Button) g_slot = h.slot;
  }
}

void on_mouse_button(int button, bool down, int x, int y) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (g_state != State::Open) return;
  if (!down) {
    g_drag_slider = false;
    return;
  }
  if (g_capturing) {
    capture_input(bindings::kMouse + u32(button));
    return;
  }
  if (button == 1) {  // right click: back, as Circle / Escape
    close_locked();
    return;
  }
  if (button != 0) return;
  update_client_size();
  Hit h = hit_test(x, y);
  switch (h.what) {
    case Hit::Tab:
      set_page(h.index);
      break;
    case Hit::Button:
      g_focus = h.index;
      g_slot = h.slot;
      activate();
      break;
    case Hit::Slot:
      g_focus = h.index;
      g_slot = h.slot;
      g_capturing = true;
      break;
    case Hit::ArrowL:
    case Hit::ArrowR:
      g_focus = h.index;
      horizontal(h.what == Hit::ArrowL ? -1 : 1);
      break;
    case Hit::Bar: {
      g_focus = h.index;
      Row& r = g_rows[size_t(g_focus)];
      apply_slider(r, r.lo + (r.hi - r.lo) * std::clamp((h.x - kBarL) / (kBarR - kBarL), 0.0f, 1.0f));
      g_drag_slider = true;
      break;
    }
    case Hit::Row:
      g_focus = h.index;
      if (g_rows[size_t(g_focus)].kind == Kind::Choice) activate();
      break;
    default:
      break;
  }
}

void on_wheel(int delta) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (g_state != State::Open) return;
  if (g_capturing) {
    capture_input(delta > 0 ? bindings::kWheelUp : bindings::kWheelDown);
    return;
  }
  g_scroll -= delta / WHEEL_DELTA;
  int n = list_rows();
  g_scroll = std::clamp(g_scroll, 0, std::max(0, n - kVisibleRows));
}

void tick() {
  if (g_open_request.exchange(false)) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    update_client_size();
    if (g_state == State::Closed) open_locked();
  }
  if (g_state.load() == State::Closed) return;
  u16 buttons = 0;
  float lx = 0, ly = 0;
  if (!input_isolated())
    for (int p = 0; p < pads::kSlots; ++p) {
      XINPUT_STATE xs{};
      if (!pads::get(p, &xs)) continue;
      buttons |= xs.Gamepad.wButtons;
      float x = xs.Gamepad.sThumbLX / 32768.0f, y = xs.Gamepad.sThumbLY / 32768.0f;
      if (std::fabs(x) + std::fabs(y) > std::fabs(lx) + std::fabs(ly)) lx = x, ly = y;
    }
  u16 pressed = u16(buttons & ~g_pad_prev);
  g_pad_prev = buttons;
  int stick = (ly > 0.55f ? 1 : 0) | (ly < -0.55f ? 2 : 0) | (lx < -0.55f ? 4 : 0) | (lx > 0.55f ? 8 : 0);
  int stick_pressed = stick & ~g_stick_prev;
  g_stick_prev = stick;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_pad_held = buttons;
  if (g_capturing) {  // bindings are keyboard and mouse inputs: the controller can only cancel
    if (pressed & XINPUT_GAMEPAD_B) g_capturing = false;
    return;
  }
  // Directions: D-pad or left stick, repeating while held.
  int dir = (buttons & XINPUT_GAMEPAD_DPAD_UP || stick & 1)      ? 1
            : (buttons & XINPUT_GAMEPAD_DPAD_DOWN || stick & 2)  ? 2
            : (buttons & XINPUT_GAMEPAD_DPAD_LEFT || stick & 4)  ? 3
            : (buttons & XINPUT_GAMEPAD_DPAD_RIGHT || stick & 8) ? 4
                                                                 : 0;
  bool fresh = (pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) ||
               stick_pressed;
  auto now = Clock::now();
  bool fire = false;
  if (dir && (fresh || dir != g_repeat_dir)) {
    fire = true;
    g_repeat_at = now + std::chrono::milliseconds(380);
  } else if (dir && now >= g_repeat_at) {
    fire = true;
    g_repeat_at = now + std::chrono::milliseconds(90);
  }
  g_repeat_dir = dir;
  if (fire) {
    if (dir == 1) move_focus(-1);
    else if (dir == 2) move_focus(1);
    else horizontal(dir == 3 ? -1 : 1);
  }
  if (pressed & XINPUT_GAMEPAD_A) activate();
  if (pressed & XINPUT_GAMEPAD_X) clear_slot();
  if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) set_page(g_page - 1);
  if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) set_page(g_page + 1);
  if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) close_locked();
}

u16 pad_suppressed(u32 port, u16 buttons) {
  if (port >= 4) return buttons;
  u16 s = g_suppress[port].load(std::memory_order_relaxed);
  if (!s) return buttons;
  s &= buttons;  // released buttons return to the game
  g_suppress[port].store(s, std::memory_order_relaxed);
  return u16(buttons & ~s);
}

// ---------------------------------------------------------------------------------------------
// Drawing (presenting thread): Direct2D/DirectWrite on the swap chain's back buffer. Its D3D11
// pipeline state is kept apart from the renderer's (SwapDeviceContextState).
namespace {

struct Gfx {
  bool tried = false, ok = false;
  ID2D1Factory1* factory = nullptr;
  ID2D1Device* device = nullptr;
  ID2D1DeviceContext* dc = nullptr;
  IDWriteFactory* dw = nullptr;
  IDWriteTextFormat *title = nullptr, *subtitle = nullptr, *label = nullptr, *value = nullptr, *small = nullptr, *help = nullptr,
                    *tab = nullptr, *button = nullptr;
  // System dialogs: heading, wrapped text, list rows, dates, on-screen keys.
  IDWriteTextFormat *dlg_title = nullptr, *body = nullptr, *row = nullptr, *date = nullptr, *key = nullptr;
  ID2D1SolidColorBrush* brush = nullptr;
  ID2D1LinearGradientBrush* select = nullptr;
  ID3DDeviceContextState* state = nullptr;
};
Gfx g_gfx;

IDWriteTextFormat* text_format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
  IDWriteTextFormat* f = nullptr;
  if (FAILED(g_gfx.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
    return nullptr;
  f->SetTextAlignment(align);
  f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return f;
}

bool has_font(const wchar_t* family) {
  IDWriteFontCollection* fc = nullptr;
  if (FAILED(g_gfx.dw->GetSystemFontCollection(&fc, FALSE)) || !fc) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  fc->FindFamilyName(family, &index, &exists);
  fc->Release();
  return exists;
}

bool init_gfx(ID3D11Device* dev) {
  g_gfx.tried = true;
  D2D1_FACTORY_OPTIONS fo{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(&g_gfx.factory))))
    return false;
  IDXGIDevice* dxgi = nullptr;
  if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) return false;
  HRESULT hr = g_gfx.factory->CreateDevice(dxgi, &g_gfx.device);
  dxgi->Release();
  if (FAILED(hr) || FAILED(g_gfx.device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_gfx.dc))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_gfx.dw)))) return false;
  ID3D11Device1* dev1 = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_gfx.state);
    dev1->Release();
  }
  if (!g_gfx.state) return false;
  // The game's option screens use a classical serif for captions and a plain sans for values:
  // Times New Roman and Bahnschrift (Segoe UI where missing), from the system's own fonts.
  const wchar_t* serif = L"Times New Roman";
  const wchar_t* sans = has_font(L"Bahnschrift") ? L"Bahnschrift" : L"Segoe UI";
  g_gfx.title = text_format(serif, 40, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
  g_gfx.subtitle = text_format(serif, 25, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
  g_gfx.label = text_format(serif, 25, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
  g_gfx.value = text_format(sans, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
  g_gfx.small = text_format(sans, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
  g_gfx.help = text_format(sans, 20, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
  g_gfx.tab = text_format(sans, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
  g_gfx.button = text_format(serif, 25, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
  g_gfx.dlg_title = text_format(serif, 34, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
  g_gfx.body = text_format(sans, 19, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
  if (g_gfx.body) {
    g_gfx.body->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    g_gfx.body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  }
  g_gfx.row = text_format(sans, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
  g_gfx.date = text_format(sans, 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
  g_gfx.key = text_format(sans, 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
  if (!g_gfx.title || !g_gfx.subtitle || !g_gfx.label || !g_gfx.value || !g_gfx.small || !g_gfx.help || !g_gfx.tab || !g_gfx.button ||
      !g_gfx.dlg_title || !g_gfx.body || !g_gfx.row || !g_gfx.date || !g_gfx.key)
    return false;
  g_gfx.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_gfx.brush);
  D2D1_GRADIENT_STOP stops[2] = {{0, D2D1::ColorF(0.27f, 0.32f, 0.37f, 1)}, {1, D2D1::ColorF(0.10f, 0.12f, 0.15f, 1)}};
  ID2D1GradientStopCollection* sc = nullptr;
  g_gfx.dc->CreateGradientStopCollection(stops, 2, &sc);
  if (sc) {
    g_gfx.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}), sc, &g_gfx.select);
    sc->Release();
  }
  if (!g_gfx.brush || !g_gfx.select) return false;
  LOG_I("ui", "PC SETTINGS drawing ready (Direct2D; fonts %ls / %ls)", serif, sans);
  return true;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void color(float g, float a = 1) { g_gfx.brush->SetColor(D2D1::ColorF(g, g, g, a)); }

void text(const std::string& s, IDWriteTextFormat* f, float x0, float y0, float x1, float y1, float g) {
  color(g);
  std::wstring w = widen(s);
  // A line wider than its box is set smaller until it fits (long captions and tags), not cut off.
  IDWriteTextLayout* tl = nullptr;
  if (x1 > x0 && y1 > y0 && f->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP &&
      SUCCEEDED(g_gfx.dw->CreateTextLayout(w.c_str(), UINT32(w.size()), f, x1 - x0, y1 - y0, &tl))) {
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(tl->GetMetrics(&m)) && m.widthIncludingTrailingWhitespace > x1 - x0)
      tl->SetFontSize(f->GetFontSize() * 0.98f * (x1 - x0) / m.widthIncludingTrailingWhitespace, {0, UINT32(w.size())});
    g_gfx.dc->DrawTextLayout({x0, y0}, tl, g_gfx.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    tl->Release();
    return;
  }
  g_gfx.dc->DrawText(w.c_str(), UINT32(w.size()), f, D2D1::RectF(x0, y0, x1, y1), g_gfx.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void line(float x0, float y0, float x1, float y1, float g) {
  color(g);
  g_gfx.dc->DrawLine({x0, y0}, {x1, y1}, g_gfx.brush, 1.0f);
}

void frame(float x0, float y0, float x1, float y1, float outer, float inner) {
  color(outer);
  g_gfx.dc->DrawRectangle(D2D1::RectF(x0 + 0.5f, y0 + 0.5f, x1 - 0.5f, y1 - 0.5f), g_gfx.brush, 1.0f);
  color(inner);
  g_gfx.dc->DrawRectangle(D2D1::RectF(x0 + 3.5f, y0 + 3.5f, x1 - 3.5f, y1 - 3.5f), g_gfx.brush, 1.0f);
}

void selected_fill(float x0, float y0, float x1, float y1) {
  g_gfx.select->SetStartPoint({0, y0});
  g_gfx.select->SetEndPoint({0, y1});
  g_gfx.dc->FillRectangle(D2D1::RectF(x0, y0, x1, y1), g_gfx.select);
}

// A small filled triangle pointing left, right, up or down (dx, dy: unit direction).
void triangle(float cx, float cy, float dx, float dy, float g) {
  ID2D1PathGeometry* path = nullptr;
  if (FAILED(g_gfx.factory->CreatePathGeometry(&path))) return;
  ID2D1GeometrySink* sink = nullptr;
  if (SUCCEEDED(path->Open(&sink))) {
    float px = -dy, py = dx;  // perpendicular
    sink->BeginFigure({cx - 5 * dx + 8 * px, cy - 5 * dy + 8 * py}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({cx + 5 * dx, cy + 5 * dy});
    sink->AddLine({cx - 5 * dx - 8 * px, cy - 5 * dy - 8 * py});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    color(g);
    g_gfx.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_gfx.dc->FillGeometry(path, g_gfx.brush);
    g_gfx.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  }
  path->Release();
}

struct Snapshot {
  std::vector<Row> rows;
  int page = 0, focus = 0, slot = 0, scroll = 0;
  bool capturing = false;
  std::string toast;
};

void draw(const Snapshot& s, u32 w, u32 h) {
  ID2D1DeviceContext* dc = g_gfx.dc;
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  color(0);  // a page of its own, as the game's settings screens
  dc->FillRectangle(D2D1::RectF(0, 0, float(w), float(h)), g_gfx.brush);
  Canvas c = canvas_for(int(w), int(h));
  dc->SetTransform(D2D1::Matrix3x2F::Scale(c.scale, c.scale) * D2D1::Matrix3x2F::Translation(c.ox, c.oy));

  // Title block (as SYSTEM / GAME SETTINGS) and the line under it.
  text("PC SETTINGS", g_gfx.title, 0, 34, 326, 76, 0.86f);
  text(kPages[s.page], g_gfx.subtitle, 0, 74, 326, 101, 0.86f);
  line(0, 74.5f, 348, 74.5f, 0.62f);
  line(348.5f, 0, 348.5f, 101, 0.62f);
  line(0, 101.5f, 348, 101.5f, 0.62f);
  line(364, 101.5f, kRowR + 1, 101.5f, 0.62f);
  // Page tabs (top right).
  for (int t = 0; t < kPageCount; ++t) {
    float x0 = kTabX[t], x1 = kTabX[t] + kTabW[t];
    if (t == s.page) selected_fill(x0, kTabY, x1, kTabY + kTabH);
    color(t == s.page ? 0.7f : 0.38f);
    dc->DrawRectangle(D2D1::RectF(x0 + 0.5f, kTabY + 0.5f, x1 - 0.5f, kTabY + kTabH - 0.5f), g_gfx.brush, 1.0f);
    text(kPages[t], g_gfx.tab, x0, kTabY, x1, kTabY + kTabH, t == s.page ? 0.9f : 0.5f);
  }

  // Rows.
  int n = int(s.rows.size()) - 1;
  for (int v = 0; v < kVisibleRows && s.scroll + v < n; ++v) {
    int i = s.scroll + v;
    const Row& r = s.rows[size_t(i)];
    float y0 = kListTop + float(v) * kPitch, y1 = y0 + kRowH, ym = (y0 + y1) / 2;
    bool focused = i == s.focus;
    bool bind = r.kind == Kind::Binding;
    float divider = bind ? kBindDivider : 332;
    if (focused) selected_fill(kRowL + 1, y0 + 1, bind ? divider : kRowR - 1, y1 - 1);
    frame(kRowL, y0, kRowR, y1, 0.55f, 0.28f);
    float g = r.enabled ? 0.83f : 0.42f;
    text(r.label, g_gfx.label, 94, y0, divider - 6, y1, g);
    line(divider + 0.5f, y0 + 7, divider + 0.5f, y1 - 7, 0.4f);
    if (r.kind == Kind::Choice) {
      triangle(kArrowL, ym, -1, 0, r.enabled && r.index > 0 ? 0.6f : 0.25f);
      triangle(kArrowR, ym, 1, 0, r.enabled && r.index + 1 < int(r.options.size()) ? 0.6f : 0.25f);
      if (r.index >= 0 && r.index < int(r.options.size())) text(r.options[size_t(r.index)], g_gfx.value, kBarL, y0, kBarR, y1, g);
    } else if (r.kind == Kind::Slider) {
      text(r.value_note, g_gfx.value, 336, y0, 440, y1, 0.8f);
      if (!r.side_note.empty()) text(r.side_note, g_gfx.value, kArrowR + 16, y0, kRowR - 8, y1, 0.8f);
      triangle(kArrowL, ym, -1, 0, 0.6f);
      triangle(kArrowR, ym, 1, 0, 0.6f);
      float f = float((r.value - r.lo) / (r.hi - r.lo));
      color(0.3f);
      dc->FillRectangle(D2D1::RectF(kBarL, ym - 5, kBarL + (kBarR - kBarL) * f, ym + 6), g_gfx.brush);
      color(0.5f);
      dc->DrawRectangle(D2D1::RectF(kBarL + 0.5f, ym - 5.5f, kBarR - 0.5f, ym + 5.5f), g_gfx.brush, 1.0f);
    } else if (bind) {
      text(r.tag, g_gfx.small, divider + 4, y0, kSlotX[0] - 10, y1, 0.55f);
      for (int k = 0; k < 2; ++k) {
        float x0 = kSlotX[k], x1 = kSlotX[k] + kSlotW;
        bool on = focused && s.slot == k;
        if (on) selected_fill(x0 + 1, y0 + 5, x1 - 1, y1 - 5);
        color(on ? 0.75f : 0.35f);
        dc->DrawRectangle(D2D1::RectF(x0 + 0.5f, y0 + 5.5f, x1 - 0.5f, y1 - 5.5f), g_gfx.brush, 1.0f);
        bool waiting = on && s.capturing;
        std::string t = waiting ? "PRESS A KEY" : r.slots[k].empty() ? "-" : r.slots[k];
        text(t, g_gfx.value, x0, y0, x1, y1, r.slots[k].empty() && !waiting ? 0.4f : 0.88f);
      }
    }
  }
  // More rows above / below.
  if (s.scroll > 0) triangle(kRowR - 12, kListTop - 12, 0, -1, 0.5f);
  if (s.scroll + kVisibleRows < n) triangle(kRowR - 12, kListBottom + 2, 0, 1, 0.5f);

  // Buttons.
  const char* names[2] = {"DEFAULT", "CLOSE"};
  for (int b = 0; b < 2; ++b) {
    float x0 = kButtonX[b], x1 = x0 + kButtonW;
    bool on = s.focus == n && s.slot == b;
    if (on) selected_fill(x0 + 1, kButtonsY + 1, x1 - 1, kButtonsY + kRowH - 1);
    frame(x0, kButtonsY, x1, kButtonsY + kRowH, 0.55f, 0.28f);
    text(names[b], g_gfx.button, x0, kButtonsY, x1, kButtonsY + kRowH, 0.86f);
  }

  // Help bar: messages (a moved binding, the second DEFAULT press, the keys of a rebinding), else
  // the focused row's note.
  line(0, 638.5f, kW, 638.5f, 0.62f);
  line(0, 676.5f, kW, 676.5f, 0.62f);
  std::string help = !s.toast.empty() ? s.toast
                     : s.capturing    ? "ESC  CANCEL      DELETE  CLEAR"
                     : s.focus >= 0 && s.focus < n ? s.rows[size_t(s.focus)].help
                                                    : "";
  text(help, g_gfx.help, 96, 640, kW - 40, 675, 0.9f);
}

// UTF-8 text (save titles and details carry full-width characters).
std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
void wtext(const std::wstring& w, IDWriteTextFormat* f, const Box& b, float g) {
  color(g);
  g_gfx.dc->DrawText(w.c_str(), UINT32(w.size()), f, D2D1::RectF(b.x0, b.y0, b.x1, b.y1), g_gfx.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void draw_dialog(const DialogView& v, u32 w, u32 h) {
  ID2D1DeviceContext* dc = g_gfx.dc;
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  g_gfx.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));  // the game, dimmed, behind
  dc->FillRectangle(D2D1::RectF(0, 0, float(w), float(h)), g_gfx.brush);
  Canvas c = canvas_for(int(w), int(h));
  dc->SetTransform(D2D1::Matrix3x2F::Scale(c.scale, c.scale) * D2D1::Matrix3x2F::Translation(c.ox, c.oy));
  DialogLayout L = dialog_layout(v);
  const Box& p = L.panel;
  g_gfx.brush->SetColor(D2D1::ColorF(0.035f, 0.04f, 0.05f, 0.97f));
  dc->FillRectangle(D2D1::RectF(p.x0, p.y0, p.x1, p.y1), g_gfx.brush);
  frame(p.x0, p.y0, p.x1, p.y1, 0.55f, 0.28f);
  wtext(wide(v.heading), g_gfx.dlg_title, L.heading, 0.86f);
  line(L.heading.x0, L.rule_y + 0.5f, L.heading.x1, L.rule_y + 0.5f, 0.62f);
  switch (v.kind) {
    case DialogView::List: {
      for (size_t i = 0; i < L.rows.size(); ++i) {
        int r = v.scroll + int(i);
        const Box& b = L.rows[i];
        if (r == v.focus) selected_fill(b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1);
        frame(b.x0, b.y0, b.x1, b.y1, 0.55f, 0.28f);
        wtext(wide(v.rows[size_t(r)]), g_gfx.row, Box{b.x0 + 16, b.y0, b.x1 - 200, b.y1}, 0.86f);
        if (size_t(r) < v.dates.size()) wtext(wide(v.dates[size_t(r)]), g_gfx.date, Box{b.x1 - 200, b.y0, b.x1 - 16, b.y1}, 0.62f);
      }
      if (!L.rows.empty()) {
        if (v.scroll > 0) triangle(L.rows.front().x1 + 13, L.rows.front().y0 + 12, 0, -1, 0.5f);
        if (v.scroll + kDialogListRows < int(v.rows.size())) triangle(L.rows.back().x1 + 13, L.rows.back().y1 - 12, 0, 1, 0.5f);
      }
      if (v.focus >= 0 && size_t(v.focus) < v.details.size()) wtext(wide(v.details[size_t(v.focus)]), g_gfx.body, L.detail, 0.72f);
      break;
    }
    case DialogView::Question:
    case DialogView::Message: wtext(wide(v.text), g_gfx.body, L.text, 0.86f); break;
    case DialogView::Text: {
      wtext(wide(v.text), g_gfx.body, L.text, 0.8f);
      const Box& e = L.edit;
      if (v.edit_selected) selected_fill(e.x0 + 1, e.y0 + 1, e.x1 - 1, e.y1 - 1);  // replaced by the first key
      frame(e.x0, e.y0, e.x1, e.y1, 0.55f, 0.28f);
      std::wstring typed(v.edit.begin(), v.edit.end());
      wtext(v.edit_selected ? typed : typed + L"_", g_gfx.row, Box{e.x0 + 16, e.y0, e.x1 - 110, e.y1}, 0.9f);
      wtext(std::to_wstring(v.edit.size()) + L" / " + std::to_wstring(v.max_chars), g_gfx.date, Box{e.x1 - 110, e.y0, e.x1 - 16, e.y1}, 0.55f);
      for (size_t i = 0; i < L.keys.size(); ++i) {
        const Box& b = L.keys[i];
        bool on = int(i) == v.key;
        if (on) selected_fill(b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1);
        color(on ? 0.75f : 0.35f);
        dc->DrawRectangle(D2D1::RectF(b.x0 + 0.5f, b.y0 + 0.5f, b.x1 - 0.5f, b.y1 - 0.5f), g_gfx.brush, 1.0f);
        std::wstring label = v.keys[i] == u' ' ? L"SPACE" : std::wstring(1, wchar_t(v.keys[i]));
        wtext(label, g_gfx.key, b, on ? 0.95f : 0.8f);
      }
      wtext(L"Keyboard: type, BACKSPACE deletes, ENTER is OK, ESC goes back.  Controller: D-pad and A type, X deletes, "
            L"Y adds a space, START is OK, B goes back.",
            g_gfx.body, L.help, 0.58f);
      break;
    }
  }
  static const char* const kNames[3][2] = {{"OK", "BACK"}, {"YES", "NO"}, {"OK", ""}};
  const char* const* names = kNames[v.kind == DialogView::Question ? 1 : v.kind == DialogView::Message ? 2 : 0];
  for (int b = 0; b < L.buttons_n; ++b) {
    const Box& r = L.buttons[b];
    if (v.button == b) selected_fill(r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1);
    frame(r.x0, r.y0, r.x1, r.y1, 0.55f, 0.28f);
    text(names[b], g_gfx.button, r.x0, r.y0, r.x1, r.y1, 0.86f);
  }
}

}  // namespace

void render(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, u32 w, u32 h) {
  if (!screen_up() || !backbuffer) return;
  if (!g_gfx.tried) {
    g_gfx.ok = init_gfx(dev);
    if (!g_gfx.ok) LOG_W("ui", "PC SETTINGS cannot be drawn (Direct2D/DirectWrite unavailable)");
  }
  if (!g_gfx.ok) return;
  Snapshot s;
  bool page = false;
  {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_state.load() != State::Closed) {
      page = true;
      s.rows = g_rows;
      s.page = g_page;
      s.focus = g_focus;
      s.slot = g_slot;
      s.scroll = g_scroll;
      s.capturing = g_capturing;
      if (!g_toast.empty() && Clock::now() < g_toast_until) s.toast = g_toast;
    }
  }
  if (s.rows.empty()) page = false;
  DialogView dv;
  bool dialog = sysdlg::view(&dv);  // a system dialog: over the game (and over PC SETTINGS)
  if (!page && !dialog) return;
  ID3D11DeviceContext1* ctx1 = nullptr;
  if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return;
  ID3DDeviceContextState* prev = nullptr;
  ctx1->SwapDeviceContextState(g_gfx.state, &prev);
  IDXGISurface* surface = nullptr;
  ID2D1Bitmap1* target = nullptr;
  if (SUCCEEDED(backbuffer->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(&surface)))) {
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (SUCCEEDED(g_gfx.dc->CreateBitmapFromDxgiSurface(surface, &bp, &target))) {
      g_gfx.dc->SetTarget(target);
      g_gfx.dc->BeginDraw();
      if (page) draw(s, w, h);
      if (dialog) draw_dialog(dv, w, h);
      HRESULT hr = g_gfx.dc->EndDraw();
      if (FAILED(hr)) LOG_W("ui", "PC SETTINGS drawing failed (0x%08lx)", (unsigned long)hr);
      g_gfx.dc->SetTarget(nullptr);
      target->Release();
    }
    surface->Release();
  }
  ctx1->SwapDeviceContextState(prev, nullptr);
  if (prev) prev->Release();
  ctx1->Release();
}

}  // namespace rt::ui
