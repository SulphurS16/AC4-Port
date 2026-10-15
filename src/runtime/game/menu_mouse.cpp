// Mouse and keyboard in the game's menus: interim version for Armored Core 4. The game's menus are
// the same dialog framework as ACFA's (CMenuDialog, CMenuDlgObj, CMenuSliderCtrl, CMenuListCtrl in
// the executable), but its dispatch points and layouts in BLUS30027 are not mapped yet, so the
// cursor cannot yet focus the item under it. Until then: outside missions the menu keys apply
// (bindings [menu]), the left button presses Cross, the right button Circle and the wheel the
// D-pad on whatever item the game highlights, and the system arrow cursor shows while the mouse is
// in use.
#include "runtime/game/menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

#include "runtime/game/frame_policy.h"

namespace rt::game {
namespace {

// cellPad digital words.
constexpr u16 kUp = 0x10, kDown = 0x40;           // digital 1
constexpr u16 kCircle = 0x20, kCross = 0x40;      // digital 2
constexpr int kHoldReads = 3, kGapReads = 2;      // a press lasts 3 pad reads, then 2 reads released

struct Press {
  u16 d1 = 0, d2 = 0;
};
std::mutex g_mu;
std::deque<Press> g_presses;
int g_phase = 0;  // reads left in the current press (> 0) or gap (< 0)
std::atomic<bool> g_pad_last{false};

void queue(u16 d1, u16 d2) {
  std::lock_guard lk(g_mu);
  if (g_presses.size() < 16) g_presses.push_back({d1, d2});
}

}  // namespace

InputContext input_context() { return mission_running() ? InputContext::Gameplay : InputContext::Menu; }

void menu_mouse_moved() { g_pad_last = false; }

void menu_mouse_button(int button, bool down) {
  g_pad_last = false;
  if (!down || input_context() != InputContext::Menu) return;
  queue(0, button == 0 ? kCross : kCircle);
}

void menu_mouse_wheel(int notches) {
  g_pad_last = false;
  if (input_context() != InputContext::Menu) return;
  for (int i = 0; i < std::min(notches < 0 ? -notches : notches, 8); ++i) queue(notches > 0 ? kUp : kDown, 0);
}

bool menu_cursor_visible() { return input_context() == InputContext::Menu && !g_pad_last.load(); }

void note_pad_input() { g_pad_last = true; }

bool controller_used_last() { return g_pad_last.load(); }

void menu_pad_bits(u16* d1, u16* d2) {
  std::lock_guard lk(g_mu);
  if (g_phase > 0) {
    *d1 |= g_presses.front().d1;
    *d2 |= g_presses.front().d2;
    if (--g_phase == 0) {
      g_presses.pop_front();
      g_phase = -kGapReads;
    }
  } else if (g_phase < 0) {
    ++g_phase;
  } else if (!g_presses.empty()) {
    g_phase = kHoldReads;
    *d1 |= g_presses.front().d1;
    *d2 |= g_presses.front().d2;
    --g_phase;
  }
}

}  // namespace rt::game
