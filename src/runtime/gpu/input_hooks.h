// Keyboard, mouse and focus events from the window to the input layer.
#pragma once
#include "common/bytes.h"

namespace rt::gpu {

void input_key(u32 vk, bool down);
void input_focus(bool focused);
// A raw input mouse report (RAWMOUSE fields).
void input_mouse_raw(u16 flags, s32 x, s32 y, u16 button_flags, u16 button_data);
void input_mouse_capture(bool captured);

}  // namespace rt::gpu
