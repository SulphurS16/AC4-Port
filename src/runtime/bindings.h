// Keyboard and mouse bindings onto the emulated pad (port 0), from a user-editable file
// (local\config\input.ini, written with the default layout when missing; --input-config PATH).
// The default layout follows the game's SIMPLE key scheme: WASD move, mouse look, left/right
// click right/left arm weapon, Space boost, Shift quick boost, E overed boost, Escape pause.
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt::bindings {

// The pad state the keyboard and mouse ask for; sticks are -1 when no bound key drives them.
struct PadOut {
  u16 d1 = 0, d2 = 0;  // cellPad digital words (bit layout of CELL_PAD_BTN_OFFSET_DIGITAL1/2)
  int lx = -1, ly = -1, rx = -1, ry = -1;
};

struct MouseSettings {
  double sensitivity = 1.0;  // scale on raw counts
  bool invert_y = false;
};

void load(const std::string& path);  // logs what it read; keeps the defaults on errors
u32 key_code(const std::string& name);  // input name as in the file -> code (0 if unknown)
PadOut sample();                      // current keys / mouse buttons / wheel
const MouseSettings& mouse();

}  // namespace rt::bindings
