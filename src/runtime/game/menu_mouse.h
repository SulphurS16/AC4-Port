// Mouse and keyboard in the game's own menus (menu_mouse.cpp; GAME_NOTES "Menu dialogs").
#pragma once
#include "common/bytes.h"

namespace rt::game {

// What the player's input drives right now (bindings.h chooses keys by it):
//   Menu:     a menu dialog with a focused item takes the keys (menu keys, cursor, clicks)
//   Gameplay: a mission runs and no menu is open
//   Screen:   anything else (title, movies, loading): Enter, Space, Escape and a click press Start
enum class InputContext { Gameplay, Menu, Screen };
InputContext input_context();

// Window thread: mouse activity over the client area (client pixels) while the mouse is free.
void menu_mouse_moved();
void menu_mouse_button(int button, bool down);  // 0 left, 1 right
void menu_mouse_wheel(int notches);              // + up, - down
// The menu cursor is shown: a menu is open, the mouse was used after any controller input and
// the window is in front.
bool menu_cursor_visible();
// A controller was used (sys_io.cpp): the cursor hides until the mouse moves again.
void note_pad_input();
// A controller was used after the mouse: gameplay leaves the mouse free until it moves (a player
// who used only the keyboard so far gets mouse look at once).
bool controller_used_last();

// Port 0's pad state gets the presses made with the mouse (Cross, Circle, D-pad, Start).
void menu_pad_bits(u16* d1, u16* d2);

}  // namespace rt::game
