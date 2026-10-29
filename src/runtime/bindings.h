// Keyboard and mouse bindings onto the emulated pad (port 0), kept in the [keyboard], [menu] and
// [mouse] sections of the shared settings file (common/settings_file.h; defaults: the user's layout
// on the game's TYPE A: WASD move, the mouse turns and looks, left / right click left / right arm
// weapon, Space / Shift / Ctrl boost / quick boost / over boost, Escape pause). In missions a
// button's keys do that button's TYPE A action and press the button the game's key type gives it
// (game/key_assign.h), and key-only controls press the controller's button combinations (the
// purges). In the game's menus the [menu] keys come first (Enter choose, Escape back;
// game/menu_mouse.h decides where the input goes). The PC SETTINGS screen changes them while the
// game runs.
#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

namespace ac4 {
class Ini;
}

namespace rt::bindings {

// The pad state the keyboard and mouse ask for; sticks are -1 when no bound key drives them.
struct PadOut {
  u16 d1 = 0, d2 = 0;  // cellPad digital words (bit layout of CELL_PAD_BTN_OFFSET_DIGITAL1/2)
  int lx = -1, ly = -1, rx = -1, ry = -1;
};

// How the mouse turns and looks in missions: as the controller's right stick, deflected by the
// mouse's speed so that the AC turns as far as the mouse moves (the default), or by moving the
// camera, which the AC then turns to follow (game/input_hooks.cpp).
enum class MouseMode { Stick, Camera };
const char* mouse_mode_name(MouseMode m);  // as in the file: stick, camera

struct MouseSettings {
  double sensitivity_x = 1.0, sensitivity_y = 1.0;  // scale on raw counts: turning, looking up and down
  bool invert_x = false, invert_y = false;
  MouseMode mode = MouseMode::Stick;
};

// Input codes: Windows virtual-key codes 1-255, mouse buttons kMouse + 0..4, wheel up/down.
constexpr u32 kMouse = 0x100, kWheelUp = 0x200, kWheelDown = 0x201;

// The pad controls the keyboard and mouse can drive, in settings-file order.
size_t control_count();
const char* control_name(size_t c);  // as in the settings file: "cross", "lstick_up", ...

void load(const ac4::Ini& ini);  // [keyboard] and [mouse]; logs what it read
void store(ac4::Ini& ini);       // the current bindings and mouse settings
// The two layers: gameplay ([keyboard]) and the game's menus ([menu]).
enum class Layer { Game, Menu };
std::vector<u32> get(size_t c, Layer l = Layer::Game);
void set(size_t c, const std::vector<u32>& codes, Layer l = Layer::Game);
void reset_defaults(Layer l);     // that layer's bindings (mouse settings stay)

u32 key_code(const std::string& name);  // input name as in the file -> code (0 if unknown)
std::string input_name(u32 code);       // code -> the name used in the file ("" if unknown)
// Keys the window keeps for itself (F1 PC settings, F11 fullscreen) cannot be bound.
bool bindable(u32 code);

// Current keys / mouse buttons / wheel. `deliver`: this state goes to the game (cellPadGetData):
// presses since the previous delivery count as down even if already released (input_taps_take).
PadOut sample(bool deliver);
// The pad button a control's keys press in missions under the game's current button assignment
// ("R1", ...; "" when the control is not a button action or the assignment is not known yet).
std::string mission_button(size_t c);
MouseSettings mouse();
void set_mouse(const MouseSettings& m);

}  // namespace rt::bindings
