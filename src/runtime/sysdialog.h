// System dialogs shown on behalf of system utilities (save data lists, confirmations, errors).
// On the console these are system overlays driven by the controller; here they are modal windows
// owned by the game window, usable with mouse, keyboard or the first XInput controller (D-pad or
// left stick to move, A/Cross to accept, B/Circle to go back). While one is open the game reads no
// pad input, as the system holds the controller on the console. For unattended runs,
// --dialog "answer,..." answers them in order (choose:N | choose:text=STRING (first item whose
// texts contain STRING) | choose:cancel | yes | no | ok | text:STRING | cancel).
#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

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
// Live answers (control command "dialog ANSWER,..."; same syntax as --dialog): they answer the
// dialog on screen, then the next ones, before any --dialog answers.
void queue_answers(const std::string& csv);

}  // namespace rt::sysdlg
