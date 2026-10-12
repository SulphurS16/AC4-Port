#pragma once
#include <string>

#include "common/bytes.h"

namespace rt {

bool input_key_down(u32 vk);  // Windows virtual-key code
bool input_has_focus();
// A guest thread started (scripted input can be anchored to thread starts).
void input_thread_started(const std::string& name);
// "quit" was written to the --control file.
bool input_quit_requested();

// Mouse, while the window has captured it (click into the window; F1 or focus loss releases it).
// Movement is raw input: device counts, unaffected by the Windows pointer speed and acceleration.
void input_mouse_take(s64* dx, s64* dy);  // movement since the previous call
bool input_mouse_button(int i);           // 0 left, 1 right, 2 middle, 3 back, 4 forward
s32 input_mouse_wheel_take();             // wheel notches (120 per detent) since the previous call
bool input_mouse_captured();
void input_mouse_inject(s64 dx, s64 dy);  // scripted movement (--input mx=/my=)
void input_mouse_button_inject(int i, bool down);  // scripted button state (control "key Mouse1 down")
void input_key_inject(u32 vk, bool down);           // scripted key state (control "key W down")
// --isolate-input (automated tests on a PC in use): the window's keyboard, mouse and focus events
// and XInput pads are ignored; only scripted and control-file input reaches the game.
bool input_isolated();

}  // namespace rt
