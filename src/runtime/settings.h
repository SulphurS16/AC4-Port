// The shared settings file at run time (common/settings_file.h): loaded at startup, changed by
// the PC SETTINGS screen (ui/overlay.h) and the window's own keys (F11 / Alt+Enter, resizing),
// applied at once and saved after each change.
#pragma once
#include <string>
#include <vector>

#include "common/settings_file.h"

namespace rt::settings {

// Loads (or creates/migrates) the file; notes are for the log, which may not be open yet.
void init(const std::string& file, std::vector<std::string>* notes);
// Safe mode (launcher option, --safe-mode): this run uses the default display settings in a
// 1280x720 window (60 fps, render scale 100 %, the primary monitor); the file keeps its values
// unless the player changes them in PC SETTINGS.
void use_safe_display();
const std::string& file();

ac4::settings::Display display();
// Each applies the change to the running game and saves the file.
void set_fps(double fps);  // 0: the display's refresh rate
void set_mode(ac4::settings::Mode m);
void set_window_size(int w, int h);
void set_fullscreen_mode(int w, int h, int hz);  // 0, 0, 0: the desktop's
void set_render_scale(int percent);  // the renderer picks it up at the next frame
void set_monitor(const std::string& device);  // moves the game there (exclusive fullscreen: re-entered there)
// F11 / Alt+Enter: windowed <-> the last fullscreen kind (borderless at first).
void toggle_fullscreen();
// Window thread: the window takes the configured mode (at startup; after exclusive fullscreen
// was left on the presenting thread).
void apply_window_mode();
// The user resized the window: saved, nothing to apply.
void note_window_size(int w, int h);
// Keyboard bindings or mouse settings changed (bindings.h): saved.
void save_bindings();

}  // namespace rt::settings
