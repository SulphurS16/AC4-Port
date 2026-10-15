// PC SETTINGS: the port's own settings screen (frame rate, display mode, window size, keyboard and
// mouse bindings, mouse sensitivity and inversion), drawn in the style of the game's option screens
// (black page, framed rows, serif captions). The game's SYSTEM menu opens it: the port adds a
// PC SETTINGS row under QUIT GAME (game/system_menu.cpp; every game entry stays as it is). Mouse,
// keyboard and controller navigate it; while it is open the game sees its controller at rest.
// Changes apply at once and are saved to the shared settings file (runtime/settings.h).
#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace rt::ui {

// A system dialog drawn in the game's picture (runtime/sysdialog.cpp keeps its state and input;
// this module lays it out and draws it in the style of the PC SETTINGS screen).
struct DialogView {
  enum Kind { List, Question, Message, Text } kind = Message;
  std::string heading, text;                       // UTF-8; '\n' breaks the text's lines
  std::vector<std::string> rows, dates, details;   // List: one per row
  int focus = 0, scroll = 0;                       // List: the highlighted row, the first one shown
  int button = 0;                                  // the highlighted button (0: OK / YES, 1: BACK / NO; -1: none)
  std::u16string edit;                             // Text: what was typed
  bool edit_selected = false;                      // Text: the initial text, replaced by the first key
  u32 max_chars = 0;
  std::u16string keys;                             // Text: the on-screen keyboard's characters
  int key = -1;                                    // Text: the highlighted key (-1: none)
};
constexpr int kDialogListRows = 6;  // rows shown at once
constexpr int kDialogKeyCols = 12;  // on-screen keyboard columns
struct DialogHit {
  enum What { None, Row, Button, Key } what = None;
  int index = -1;  // Row: the row's index in the list; Button: 0 / 1; Key: the key's index
};
// What is under client pixel (x, y) of the window (window thread).
DialogHit dialog_hit(const DialogView& v, int x, int y);

// Something of the port's own is on screen (PC SETTINGS or a system dialog): drawn over the frame.
bool screen_up();

bool is_open();  // any thread
// The SYSTEM menu's PC SETTINGS row was chosen: opens the screen; closing it returns to the menu.
void open_from_menu();
void close();  // window thread

// Window thread, while open: input events (client-area pixel coordinates).
bool on_key(u32 vk, bool down, bool repeat);
void on_mouse_move(int x, int y);
void on_mouse_button(int button, bool down, int x, int y);  // 0 left, 1 right, 2 middle, 3-4 side
void on_wheel(int delta);
// Window thread, every pass of the main loop: controller navigation while open.
void tick();

// What the game's controller reads (pad module): Normal; Idle (the screen holds the controller).
enum class PadMode { Normal, Idle };
PadMode pad_mode();
// Controller buttons (XInput wButtons) still held from the screen: kept from the game until released.
u16 pad_suppressed(u32 port, u16 buttons);

// Presenting thread: draws the screen over the back buffer after the game's frame.
void render(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, u32 w, u32 h);

}  // namespace rt::ui
