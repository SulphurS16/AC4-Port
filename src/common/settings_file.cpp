#include "common/settings_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#include "common/bytes.h"
#include "common/file.h"

namespace ac4::settings {
namespace {

const char* const kHead =
    "# ARMORED CORE 4 (native port): settings, shared by the launcher and the game's\n"
    "# PC SETTINGS screen (F1 or a controller's Guide button while playing). Command-line options\n"
    "# override them for one run.\n";

const char* const kDisplay =
    "[display]\n"
    "# Frame rate in missions: 30, 60 (as on the console), 120, 144, display (the display's refresh\n"
    "# rate) or uncapped. Menus and movies run at 60 (some menu fades count frames).\n"
    "fps = 60\n"
    "# windowed, fullscreen or borderless (borderless fullscreen); F11 or Alt+Enter switch\n"
    "mode = borderless\n"
    "# Window size in windowed mode (client area), such as 1280x720 or 1920x1080\n"
    "window = 1280x720\n"
    "# Fullscreen display mode: WIDTHxHEIGHT@HZ, such as 1920x1080@60, or desktop\n"
    "fullscreen_mode = desktop\n"
    "# Monitor for the game (its device name, such as \\\\.\\DISPLAY2; empty: the primary monitor)\n"
    "monitor =\n"
    "# Render scale in percent of the output resolution (50 to 200): the game renders its 3D scene and\n"
    "# its interface at the window's or screen's resolution times this\n"
    "render_scale = 100\n"
    "# Source of the game's vertical blank: auto (the display's own when its refresh is a whole\n"
    "# multiple of fps, else a clock, with tearing allowed for variable-refresh displays), display\n"
    "# or clock\n"
    "pacing = auto\n";

const char* const kMouse =
    "[mouse]\n"
    "# Rotation per raw mouse count relative to the default (1.0): x turns, y looks up and down.\n"
    "# Raw input: no acceleration, smoothing or dead zone.\n"
    "sensitivity_x = 1.0\n"
    "sensitivity_y = 1.0\n"
    "invert_x = false\n"
    "invert_y = false\n";

const char* const kKeyboardHead =
    "[keyboard]\n"
    "# Keyboard and mouse bindings onto the controller: <control> = <input>[, <input>...]\n"
    "# Controls: cross circle square triangle l1 l2 l3 r1 r2 r3 start select up down left right\n"
    "#   lstick_up lstick_down lstick_left lstick_right rstick_up rstick_down rstick_left rstick_right\n"
    "# Inputs: A-Z, 0-9, F2-F10, F12, Num0-Num9, Space, Enter, Escape, Tab, Backspace, Shift, Ctrl,\n"
    "#   Alt, Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert, Delete, CapsLock, Mouse1\n"
    "#   (left), Mouse2 (right), Mouse3 (middle), Mouse4, Mouse5, WheelUp, WheelDown\n"
    "# Mouse buttons and the wheel count while the game window has the mouse (click into it). The\n"
    "# defaults follow the game's control TYPE A; the mouse turns and looks.\n";

const char* const kKeyboardDefaults =
    "# The game's control TYPE A (SYSTEM > CONTROL OPTIONS > KEY CONFIGURATION).\n"
    "# MOVE (left stick)\n"
    "lstick_up = W\n"
    "lstick_down = S\n"
    "lstick_left = A\n"
    "lstick_right = D\n"
    "# SIGHT (right stick: turn, look up and down; R3 resets the view); the mouse turns and looks\n"
    "rstick_up = Num8\n"
    "rstick_down = Num5, Num2\n"
    "rstick_left = Num4\n"
    "rstick_right = Num6\n"
    "r3 = C\n"
    "# BOOST: boost (hold to rise), quick boost, overed boost\n"
    "l2 = Space\n"
    "r2 = Shift\n"
    "triangle = E\n"
    "# WEAPON: right arm, left arm, shoulder; switch right / left arm weapon (hold: reload)\n"
    "cross = Mouse1, Enter\n"
    "square = Mouse2\n"
    "circle = F, Mouse3, Backspace\n"
    "r1 = R, WheelUp\n"
    "l1 = Q, WheelDown\n"
    "# LOCK: lock on target (hold to switch the lock mode)\n"
    "l3 = Ctrl, Mouse4\n"
    "# Pause, Select, D-pad\n"
    "start = Escape\n"
    "select = Tab\n"
    "up = Up\n"
    "down = Down\n"
    "left = Left\n"
    "right = Right\n";

const char* const kMenuHead =
    "[menu]\n"
    "# Keys in the game's menus (where a menu item is highlighted), as in [keyboard]: an input listed\n"
    "# here drives this control in menus; inputs not listed keep their [keyboard] control there. The\n"
    "# mouse points and clicks (left: choose, right: back, wheel: up and down). On screens without a\n"
    "# menu (title, movies) the confirm and back keys press Start.\n";

const char* const kMenuDefaults =
    "cross = Enter, Space\n"
    "circle = Escape, Backspace\n"
    "start = Tab\n"
    "up = Up, W\n"
    "down = Down, S\n"
    "left = Left, A\n"
    "right = Right, D\n"
    "l1 = Q\n"
    "r1 = E\n";

bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

}  // namespace

std::filesystem::path file_in(const std::filesystem::path& data_dir) { return data_dir / "config" / "settings.ini"; }

const char* default_keyboard_text() { return kKeyboardDefaults; }
const char* default_menu_text() { return kMenuDefaults; }

Ini load_or_create(const std::filesystem::path& file, std::vector<std::string>* notes) {
  auto note = [&](const std::string& s) {
    if (notes) notes->push_back(s);
  };
  Ini ini = Ini::load(file);
  bool changed = false;
  std::error_code ec;
  // Layout before the launcher: fps/pacing/fullscreen at the top of settings.ini, bindings and
  // mouse settings in input.ini beside it.
  bool old_layout = !ini.empty() && !ini.has_section("display") &&
                    (ini.get("", "fps") || ini.get("", "pacing") || ini.get("", "fullscreen"));
  if (ini.empty() || old_layout) {
    std::filesystem::path old_input = file.parent_path() / "input.ini";
    std::string keyboard = kKeyboardDefaults;
    std::vector<std::pair<std::string, std::string>> mouse;
    bool imported = false;
    if (std::filesystem::exists(old_input, ec)) {
      try {
        Ini in = Ini::load(old_input);
        if (auto keys = in.entries(""); !keys.empty()) {
          keyboard.clear();
          for (const auto& [k, v] : keys) keyboard += k + " = " + v + "\n";
        }
        mouse = in.entries("mouse");
        imported = true;
      } catch (const std::exception& e) {
        note(std::string("settings: input.ini not imported: ") + e.what());
      }
    }
    Ini fresh = Ini::parse(std::string(kHead) + "\n" + kDisplay + "\n" + kMouse + "\n" + kKeyboardHead + keyboard + "\n" + kMenuHead +
                           kMenuDefaults);
    for (const auto& [k, v] : mouse) fresh.set("mouse", k, v);
    if (old_layout) {
      if (auto v = ini.get("", "fps")) fresh.set("display", "fps", *v);
      if (auto v = ini.get("", "pacing")) fresh.set("display", "pacing", *v);
      if (auto v = ini.get("", "fullscreen")) fresh.set("display", "mode", truthy(*v) ? "borderless" : "windowed");
      try {
        std::filesystem::path kept = file;
        kept += ".migrated";
        write_text(kept, ini.text());
      } catch (const std::exception&) {
      }
      note("settings: fps/pacing/fullscreen migrated from the earlier layout (old file kept as settings.ini.migrated)");
    }
    if (imported) {
      std::filesystem::path done = old_input;
      done += ".migrated";
      try {
        guard_write(done);
        std::filesystem::rename(old_input, done, ec);
      } catch (const std::exception&) {
      }
      note("settings: keyboard bindings and mouse settings imported from input.ini (renamed to input.ini.migrated)");
    }
    if (ini.empty() && !old_layout) note("settings: wrote the defaults to " + file.string());
    ini = std::move(fresh);
    changed = true;
  } else {
    // A file written by the launcher before the game ever ran may lack sections: add them.
    auto add_missing = [&](const char* name, const std::string& text) {
      if (ini.has_section(name)) return;
      Ini d = Ini::parse(text);
      for (const auto& [k, v] : d.entries(name)) ini.set(name, k, v);
      changed = true;
    };
    add_missing("display", kDisplay);
    add_missing("mouse", kMouse);
    add_missing("keyboard", std::string(kKeyboardHead) + kKeyboardDefaults);
    if (!ini.has_section("menu")) {  // files from before the menu keys: the section with its notes
      std::string head = kMenuHead;
      for (size_t a = head.find('\n') + 1; a < head.size();) {
        size_t b = head.find('\n', a);
        ini.add_line("menu", head.substr(a, b - a));
        a = b + 1;
      }
      Ini d = Ini::parse(std::string(kMenuHead) + kMenuDefaults);
      for (const auto& [k, v] : d.entries("menu")) ini.set("menu", k, v);
      changed = true;
    }
  }
  if (changed) {
    try {
      ini.save(file);
    } catch (const std::exception& e) {
      note(std::string("settings: not saved: ") + e.what());
    }
  }
  return ini;
}

Display read_display(const Ini& ini) {
  Display d;
  std::string fps = ini.get_or("display", "fps", "60");
  if (fps == "display") d.fps = 0;
  else if (fps == "uncapped") d.fps = kFpsUncapped;
  else if (double v = std::atof(fps.c_str()); v >= 30 && v <= 1000) d.fps = v;
  std::string pacing = ini.get_or("display", "pacing", "auto");
  if (pacing == "auto" || pacing == "display" || pacing == "clock") d.pacing = pacing;
  std::string mode = ini.get_or("display", "mode", "windowed");
  d.mode = mode == "borderless" ? Mode::Borderless : mode == "fullscreen" ? Mode::Fullscreen : Mode::Windowed;
  std::string fs = ini.get_or("display", "fullscreen_mode", "desktop");
  int fw = 0, fh = 0, hz = 0;
  if (std::sscanf(fs.c_str(), "%dx%d@%d", &fw, &fh, &hz) >= 2 && fw >= 640 && fh >= 360 && fw <= 16384 && fh <= 16384) {
    d.fs_width = fw;
    d.fs_height = fh;
    d.fs_hz = hz > 0 && hz <= 1000 ? hz : 0;
  }
  if (int v = std::atoi(ini.get_or("display", "render_scale", "100").c_str()); v >= kRenderScaleMin && v <= kRenderScaleMax)
    d.render_scale = v;
  d.monitor = ini.get_or("display", "monitor", "");
  std::string win = ini.get_or("display", "window", "1280x720");
  int w = 0, h = 0;
  if (std::sscanf(win.c_str(), "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 16384 && h <= 16384) {
    d.width = w;
    d.height = h;
  }
  return d;
}

void write_display(Ini& ini, const Display& d) {
  ini.set("display", "fps", fps_text(d.fps));
  ini.set("display", "pacing", d.pacing);
  ini.set("display", "mode", mode_name(d.mode));
  ini.set("display", "window", strprintf("%dx%d", d.width, d.height));
  ini.set("display", "fullscreen_mode", fullscreen_text(d));
  ini.set("display", "render_scale", std::to_string(d.render_scale));
  ini.set("display", "monitor", d.monitor);
}

const char* mode_name(Mode m) { return m == Mode::Borderless ? "borderless" : m == Mode::Fullscreen ? "fullscreen" : "windowed"; }

std::string fullscreen_text(const Display& d) {
  if (!d.fs_width) return "desktop";
  return d.fs_hz ? strprintf("%dx%d@%d", d.fs_width, d.fs_height, d.fs_hz) : strprintf("%dx%d", d.fs_width, d.fs_height);
}

const std::vector<FpsChoice>& fps_choices() {
  static const std::vector<FpsChoice> v = {
      {30, "30"}, {60, "60"}, {120, "120"}, {144, "144"}, {0, "Display refresh rate"}, {kFpsUncapped, "Uncapped"},
  };
  return v;
}

std::string fps_text(double fps) {
  if (fps < 0) return "uncapped";
  if (fps == 0) return "display";
  return std::fabs(fps - std::round(fps)) < 1e-6 ? strprintf("%.0f", fps) : strprintf("%.2f", fps);
}

const std::vector<Size>& window_sizes() {
  static const std::vector<Size> v = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
  return v;
}

}  // namespace ac4::settings
