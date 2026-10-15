// The shared settings file of the launcher and the game (<data>\config\settings.ini): display
// settings, mouse settings and keyboard bindings. Both sides change single values in place
// (ac4::Ini), so neither overwrites what the other set. Older layouts are migrated on load:
// top-level fps/pacing/fullscreen keys (settings.ini before the launcher) and input.ini beside
// it (bindings and mouse settings), which is renamed to input.ini.migrated afterwards.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "common/ini.h"

namespace ac4::settings {

std::filesystem::path file_in(const std::filesystem::path& data_dir);  // <data>\config\settings.ini

// Loads `file`, migrating older layouts and adding missing sections with their defaults; when
// anything was added or migrated the result is saved. Notes describe what happened (for logs).
Ini load_or_create(const std::filesystem::path& file, std::vector<std::string>* notes);

// The default [keyboard] lines (the game's SIMPLE layout), shared with the runtime's bindings.
const char* default_keyboard_text();
const char* default_menu_text();  // [menu]: keys in the game's menus

enum class Mode { Windowed, Fullscreen, Borderless };
const char* mode_name(Mode m);  // as in the file: windowed, fullscreen, borderless

struct Display {
  double fps = 60;              // frame rate target in missions; 0 = the display's refresh rate, kFpsUncapped
  std::string pacing = "auto";  // auto | display | clock
  Mode mode = Mode::Borderless;  // fullscreen: exclusive, in the display mode below; borderless: the monitor as it is
  int width = 1280, height = 720;  // window client size (windowed)
  int fs_width = 0, fs_height = 0, fs_hz = 0;  // fullscreen display mode (0: the desktop's resolution / refresh rate)
  // The game renders at the output resolution (the 16:9 picture's size in the window or on the
  // screen) times this percentage: 100 = the output's own pixels.
  int render_scale = 100;
  std::string monitor;  // fullscreen / borderless / the window's monitor: its device name ("" = the primary)
};
constexpr int kRenderScaleMin = 50, kRenderScaleMax = 200;
Display read_display(const Ini& ini);
void write_display(Ini& ini, const Display& d);
std::string fullscreen_text(const Display& d);  // "desktop", "1920x1080@60"

// Frame rate choices offered by the launcher and the PC SETTINGS screen, in display order: only
// rates checked by game execution (movement, turn rate and fire rate in a mission match 60 fps;
// docs/STATUS.md). A rate set in the file by hand is shown as untested.
constexpr double kFpsUncapped = -1;  // no frame rate limit in missions
struct FpsChoice {
  double fps;  // 0 = display refresh, kFpsUncapped
  const char* label;
};
const std::vector<FpsChoice>& fps_choices();
std::string fps_text(double fps);  // "60", "display", "uncapped"

// Window sizes offered for windowed mode.
struct Size {
  int w, h;
};
const std::vector<Size>& window_sizes();

}  // namespace ac4::settings
