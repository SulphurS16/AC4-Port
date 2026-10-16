// System dialogs shown on behalf of system utilities (save data lists, confirmations, errors, the
// on-screen keyboard's text entry). On the console these are system overlays driven by the
// controller; here they are drawn in the game's picture (ui/overlay.cpp draws them), so the game
// window keeps its mode, size, position and focus (exclusive fullscreen stays). Mouse, keyboard or
// the first XInput controller answer them (D-pad or left stick to move, A/Cross to accept,
// B/Circle to go back). While one is open the game reads no pad input, as the system holds the
// controller on the console. For unattended runs, --dialog "answer,..." answers them in order
// (choose:N | choose:text=STRING (first item whose texts contain STRING) | choose:cancel | yes | no
// | ok | text:STRING | cancel).
#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::ui {
struct DialogView;
}

namespace rt::sysdlg {

struct ListItem {
  std::string title, subtitle, detail;
  s64 mtime = 0;  // seconds since 1970 (0: not shown)
  bool is_new = false;
};

// Modal list; returns the chosen index or -1 when the user backs out.
int choose(const std::string& heading, const std::vector<ListItem>& items, int focus);
// Modal yes/no question; true for yes.
bool confirm(const std::string& text);
// Modal message with one button.
void message(const std::string& text);
// Modal text entry (the on-screen keyboard's stand-in): returns false when the user backs out or
// the dialog is aborted. Text is UTF-16; at most max_chars characters.
bool text_input(const std::string& prompt, const std::u16string& initial, u32 max_chars, std::u16string* out);
// Closes the open dialog as if the user backed out (from another thread).
void abort();
// A dialog is open (the pad is held by the system).
bool active();
// How many dialogs have given the controller back so far: the game clears its own pad data while
// the system holds the controller, so cellPadGetData reports in full after each (sys_io.cpp).
u32 released_count();

// The dialog on screen (window thread: the window procedure routes the input here while one is
// shown). Keys and typed characters, the mouse in client pixels, the controller once per pass of
// the window's loop (tick).
bool shown();
void on_key(u32 vk, bool down, bool repeat);
void on_char(u32 ch);
void on_mouse_move(int x, int y);
void on_mouse_button(int button, bool down, int x, int y);  // 0 left, 1 right
void on_wheel(int delta);
void tick();
// The mouse was used in the dialog last (its cursor shows; keys or a controller hide it).
bool cursor_wanted();
// Presenting thread: a copy of the dialog on screen for drawing; false when none is.
bool view(ui::DialogView* out);

}  // namespace rt::sysdlg
