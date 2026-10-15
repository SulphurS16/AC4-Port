#include "runtime/sysdialog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>

#include "runtime/config.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/ui/overlay.h"

namespace rt::sysdlg {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int> g_active{0};
std::mutex g_mu;  // one dialog at a time

// ---- scripted answers (--dialog, and live "dialog ..." control commands) ----
bool take_live(std::string*) { return false; }  // player builds: no scripted answers
bool scripted(std::string*) { return false; }

std::u16string utf16(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return std::u16string(w.begin(), w.end());
}

std::string date_text(s64 t) {
  if (!t) return "";
  time_t tt = time_t(t);
  tm lt{};
  localtime_s(&lt, &tt);
  char b[32];
  std::strftime(b, sizeof b, "%Y/%m/%d %H:%M", &lt);
  return b;
}

// ---- controller ----
using XGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XGetState xinput() {
  static XGetState fn = [] {
    HMODULE h = LoadLibraryW(L"xinput1_4.dll");
    if (!h) h = LoadLibraryW(L"xinput9_1_0.dll");
    return h ? reinterpret_cast<XGetState>(reinterpret_cast<void*>(GetProcAddress(h, "XInputGetState"))) : nullptr;
  }();
  return fn;
}
enum Pad : u32 { P_UP = 1, P_DOWN = 2, P_LEFT = 4, P_RIGHT = 8, P_ACCEPT = 16, P_BACK = 32, P_X = 64, P_Y = 128, P_START = 256 };
constexpr u32 kDirections = P_UP | P_DOWN | P_LEFT | P_RIGHT;
u32 pad_now() {
  XGetState get = xinput();
  if (!get || input_isolated()) return 0;
  u32 b = 0;
  for (DWORD port = 0; port < 4; ++port) {
    XINPUT_STATE xs{};
    if (get(port, &xs) != ERROR_SUCCESS) continue;
    const auto& g = xs.Gamepad;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > 16000) b |= P_UP;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -16000) b |= P_DOWN;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16000) b |= P_LEFT;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16000) b |= P_RIGHT;
    if (g.wButtons & XINPUT_GAMEPAD_A) b |= P_ACCEPT;
    if (g.wButtons & XINPUT_GAMEPAD_B) b |= P_BACK;
    if (g.wButtons & XINPUT_GAMEPAD_X) b |= P_X;
    if (g.wButtons & XINPUT_GAMEPAD_Y) b |= P_Y;
    if (g.wButtons & XINPUT_GAMEPAD_START) b |= P_START;
  }
  return b;
}

// ---- the dialog in the game's picture ----
// The on-screen keyboard (for the controller and the mouse; the keyboard types directly).
const char16_t kKeys[] = u"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.!?'&@#() ";

struct Dlg {
  ui::DialogView v;
  int result = -1;  // List: index or -1; Question: 1 yes / 0 no; Text: 1 entered / 0 back; Message: 0
  bool done = false;
  u32 pad_last = ~0u;  // controller buttons held when it opened count only after a release
  u32 repeat_dir = 0;
  Clock::time_point repeat_at;
  bool mouse = false;  // the mouse was used last: its cursor shows
  int last_click_row = -1;
  Clock::time_point last_click;
};
// The dialog on screen: shared by the waiting game thread, the window thread (input) and the
// presenting thread (view).
std::recursive_mutex g_dmu;
std::condition_variable_any g_dcv;
Dlg* g_dlg = nullptr;

void finish(Dlg* d, int result) {
  d->result = result;
  d->done = true;
  g_dcv.notify_all();
}

int rows_of(const Dlg* d) { return int(d->v.rows.size()); }

void list_move(Dlg* d, int delta) {
  int n = rows_of(d);
  if (n <= 0) return;
  ui::DialogView& v = d->v;
  v.focus = std::clamp(v.focus + delta, 0, n - 1);
  if (v.focus < v.scroll) v.scroll = v.focus;
  if (v.focus >= v.scroll + ui::kDialogListRows) v.scroll = v.focus - ui::kDialogListRows + 1;
  v.scroll = std::clamp(v.scroll, 0, std::max(0, n - ui::kDialogListRows));
}

void accept(Dlg* d) {
  switch (d->v.kind) {
    case ui::DialogView::List:
      if (rows_of(d) > 0) finish(d, d->v.focus);
      break;
    case ui::DialogView::Question: finish(d, d->v.button == 1 ? 0 : 1); break;
    case ui::DialogView::Text: finish(d, 1); break;
    case ui::DialogView::Message: finish(d, 0); break;
  }
}
void back(Dlg* d) { finish(d, d->v.kind == ui::DialogView::List ? -1 : 0); }

// The initial text starts selected (as the console's keyboard shows it): the first character or
// BACKSPACE replaces it.
void type(Dlg* d, char16_t c) {
  if (d->v.edit_selected) d->v.edit.clear();
  d->v.edit_selected = false;
  if (d->v.edit.size() < d->v.max_chars) d->v.edit.push_back(c);
}
void erase(Dlg* d) {
  if (d->v.edit_selected) d->v.edit.clear();
  d->v.edit_selected = false;
  if (!d->v.edit.empty()) d->v.edit.pop_back();
}

// The on-screen keyboard's highlight moves (wrapping in its rows and columns).
void key_move(Dlg* d, int dx, int dy) {
  int n = int(d->v.keys.size()), cols = ui::kDialogKeyCols, rows = (n + cols - 1) / cols;
  if (n <= 0) return;
  if (d->v.key < 0) {
    d->v.key = 0;
    return;
  }
  int r = d->v.key / cols, c = d->v.key % cols;
  c = (c + dx + cols) % cols;
  r = (r + dy + rows) % rows;
  d->v.key = std::min(r * cols + c, n - 1);
}

void use_keys(Dlg* d) {  // keys or a controller took over from the mouse
  if (d->mouse) {
    d->mouse = false;
    gpu::window_refresh_cursor();
  }
}

// Shows the dialog in the game's picture and waits for the answer (the calling game thread).
bool show(Dlg* d) {
  struct Hold {
    Hold() { g_active.fetch_add(1); }
    ~Hold() { g_active.fetch_sub(1); }
  } hold;
  if (!gpu::window_handle()) return false;
  d->pad_last = pad_now();
  const bool captured = gpu::window_mouse_captured();
  if (captured) gpu::window_set_mouse_capture(false);  // the dialog takes the mouse
  {
    std::unique_lock lk(g_dmu);
    g_dlg = d;
    gpu::window_refresh_cursor();
    g_dcv.wait(lk, [&] { return d->done; });
    g_dlg = nullptr;
  }
  gpu::window_refresh_cursor();
  if (captured) gpu::window_set_mouse_capture(true);
  return true;
}

}  // namespace

bool active() { return g_active.load() > 0; }

bool shown() {
  std::lock_guard lk(g_dmu);
  return g_dlg != nullptr;
}

bool cursor_wanted() {
  std::lock_guard lk(g_dmu);
  return g_dlg && g_dlg->mouse;
}

bool view(ui::DialogView* out) {
  std::lock_guard lk(g_dmu);
  if (!g_dlg) return false;
  *out = g_dlg->v;
  return true;
}

void on_key(u32 vk, bool down, bool repeat) {
  std::lock_guard lk(g_dmu);
  Dlg* d = g_dlg;
  if (!d || d->done || !down) return;
  use_keys(d);
  ui::DialogView& v = d->v;
  if (vk == VK_ESCAPE) {
    if (!repeat) back(d);  // a fresh press: a key held from the game's menu does not answer
    return;
  }
  if (vk == VK_RETURN) {
    if (!repeat) accept(d);
    return;
  }
  switch (v.kind) {
    case ui::DialogView::List:
      if (vk == VK_UP) list_move(d, -1);
      else if (vk == VK_DOWN) list_move(d, 1);
      else if (vk == VK_PRIOR) list_move(d, -ui::kDialogListRows);
      else if (vk == VK_NEXT) list_move(d, ui::kDialogListRows);
      else if (vk == VK_HOME) list_move(d, -rows_of(d));
      else if (vk == VK_END) list_move(d, rows_of(d));
      break;
    case ui::DialogView::Question:
      if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_TAB) v.button = v.button == 0 ? 1 : 0;
      break;
    case ui::DialogView::Text:
      if (vk == VK_BACK || vk == VK_DELETE) erase(d);
      break;
    case ui::DialogView::Message: break;
  }
}

void on_char(u32 ch) {
  std::lock_guard lk(g_dmu);
  Dlg* d = g_dlg;
  if (!d || d->done || d->v.kind != ui::DialogView::Text) return;
  if (ch >= 0x20 && ch < 0x7f) type(d, char16_t(ch));  // printable ASCII (BACKSPACE, ENTER, ESC: on_key)
}

void on_mouse_move(int x, int y) {
  std::lock_guard lk(g_dmu);
  Dlg* d = g_dlg;
  if (!d || d->done) return;
  if (!d->mouse) {
    d->mouse = true;
    gpu::window_refresh_cursor();
  }
  ui::DialogHit h = ui::dialog_hit(d->v, x, y);
  if (h.what == ui::DialogHit::Row) d->v.focus = h.index;
  else if (h.what == ui::DialogHit::Button) d->v.button = h.index;
  else if (h.what == ui::DialogHit::Key) d->v.key = h.index;
}

void on_mouse_button(int button, bool down, int x, int y) {
  std::lock_guard lk(g_dmu);
  Dlg* d = g_dlg;
  if (!d || d->done || !down) return;  // presses answer (a release from the game's menu does not)
  d->mouse = true;
  if (button == 1) {
    back(d);
    return;
  }
  if (button != 0) return;
  ui::DialogHit h = ui::dialog_hit(d->v, x, y);
  auto now = Clock::now();
  switch (h.what) {
    case ui::DialogHit::Row:
      d->v.focus = h.index;
      if (h.index == d->last_click_row && now - d->last_click < std::chrono::milliseconds(GetDoubleClickTime())) accept(d);
      d->last_click_row = h.index;
      d->last_click = now;
      break;
    case ui::DialogHit::Button:
      if (h.index == 0 || d->v.kind == ui::DialogView::Message) accept(d);
      else back(d);
      break;
    case ui::DialogHit::Key:
      d->v.key = h.index;
      type(d, d->v.keys[size_t(h.index)]);
      break;
    case ui::DialogHit::None: break;
  }
}

void on_wheel(int delta) {
  std::lock_guard lk(g_dmu);
  Dlg* d = g_dlg;
  if (!d || d->done || d->v.kind != ui::DialogView::List) return;
  static int rest = 0;
  rest += delta;
  int notches = rest / WHEEL_DELTA;
  rest -= notches * WHEEL_DELTA;
  if (notches) list_move(d, -notches);
}

void tick() {
  std::lock_guard lk(g_dmu);
  Dlg* d = g_dlg;
  if (!d || d->done) return;
  if (std::string ans; take_live(&ans)) {  // dev: a live answer for the dialog on screen
    LOG_I("sysdlg", "open dialog: live answer '%s'", ans.c_str());
    if (d->v.kind == ui::DialogView::Text) {
      if (ans.rfind("text:", 0) == 0) {
        d->v.edit = utf16(ans.substr(5)).substr(0, d->v.max_chars);
        finish(d, 1);
      } else {
        finish(d, 0);
      }
    } else if (d->v.kind == ui::DialogView::List) {
      int r = -1;
      if (ans.rfind("choose:text=", 0) == 0) {
        std::string want = ans.substr(12);
        for (size_t i = 0; i < d->v.rows.size() && r < 0; ++i)
          if ((d->v.rows[i] + "\n" + d->v.details[i]).find(want) != std::string::npos) r = int(i);
      } else if (ans.rfind("choose:", 0) == 0 && ans != "choose:cancel") {
        r = std::atoi(ans.c_str() + 7);
      }
      finish(d, r >= 0 && r < rows_of(d) ? r : -1);
    } else if (d->v.kind == ui::DialogView::Question) {
      finish(d, ans == "yes" ? 1 : 0);
    } else {
      finish(d, 0);
    }
    return;
  }
  u32 now = pad_now(), edge = now & ~d->pad_last;
  d->pad_last = now;
  // Directions repeat while held (as in PC SETTINGS).
  u32 dir = now & kDirections;
  bool fire = false;
  auto t = Clock::now();
  if (dir && ((edge & kDirections) || dir != d->repeat_dir)) {
    fire = true;
    d->repeat_at = t + std::chrono::milliseconds(380);
  } else if (dir && t >= d->repeat_at) {
    fire = true;
    d->repeat_at = t + std::chrono::milliseconds(90);
  }
  d->repeat_dir = dir;
  if (fire || (edge & ~kDirections)) use_keys(d);
  ui::DialogView& v = d->v;
  if (fire) {
    if (v.kind == ui::DialogView::List) list_move(d, (dir & P_UP) ? -1 : (dir & P_DOWN) ? 1 : 0);
    else if (v.kind == ui::DialogView::Question && (dir & (P_LEFT | P_RIGHT))) v.button = (dir & P_LEFT) ? 0 : 1;
    else if (v.kind == ui::DialogView::Text)
      key_move(d, (dir & P_LEFT) ? -1 : (dir & P_RIGHT) ? 1 : 0, (dir & P_UP) ? -1 : (dir & P_DOWN) ? 1 : 0);
  }
  if (v.kind == ui::DialogView::Text) {
    if (edge & P_ACCEPT) {
      if (v.key < 0) v.key = 0;
      type(d, v.keys[size_t(v.key)]);
    }
    if (edge & P_X) erase(d);
    if (edge & P_Y) type(d, u' ');
    if (edge & P_START) accept(d);
    if (edge & P_BACK) back(d);
    return;
  }
  if (edge & P_ACCEPT) accept(d);
  else if (edge & P_BACK) back(d);
}

void abort() {
  std::lock_guard lk(g_dmu);
  if (g_dlg && !g_dlg->done) back(g_dlg);
}

bool text_input(const std::string& prompt, const std::u16string& initial, u32 max_chars, std::u16string* out) {
  std::lock_guard lk(g_mu);
  std::string ans;
  if (scripted(&ans)) {
    bool ok = ans.rfind("text:", 0) == 0;
    if (ok) {
      *out = utf16(ans.substr(5));
      if (out->size() > max_chars) out->resize(max_chars);
    }
    LOG_I("sysdlg", "text input '%s': scripted answer '%s'", prompt.c_str(), ans.c_str());
    return ok;
  }
  Dlg d;
  d.v.kind = ui::DialogView::Text;
  d.v.heading = "TEXT INPUT";
  d.v.text = prompt;
  d.v.edit = initial.substr(0, max_chars);
  d.v.edit_selected = !d.v.edit.empty();
  d.v.max_chars = max_chars;
  d.v.keys = kKeys;
  d.v.button = -1;
  LOG_I("sysdlg", "text input '%s' shown", prompt.c_str());
  if (!show(&d)) {
    LOG_W("sysdlg", "text input '%s': no window to show it; answered as back", prompt.c_str());
    return false;
  }
  if (d.result != 1) {
    LOG_I("sysdlg", "text input '%s': user backed out", prompt.c_str());
    return false;
  }
  *out = d.v.edit;
  LOG_I("sysdlg", "text input '%s': %zu character(s) entered", prompt.c_str(), out->size());
  return true;
}

int choose(const std::string& heading, const std::vector<ListItem>& items, int focus) {
  std::lock_guard lk(g_mu);
  std::string ans;
  if (scripted(&ans)) {
    int r = -1;
    if (ans.rfind("choose:text=", 0) == 0) {  // the first item whose texts contain the string
      std::string want = ans.substr(12);
      for (size_t i = 0; i < items.size() && r < 0; ++i)
        if ((items[i].title + "\n" + items[i].subtitle + "\n" + items[i].detail).find(want) != std::string::npos) r = int(i);
    } else if (ans.rfind("choose:", 0) == 0 && ans != "choose:cancel") {
      r = std::atoi(ans.c_str() + 7);
    }
    if (r >= int(items.size())) r = -1;
    LOG_I("sysdlg", "list '%s' (%zu item(s)): scripted answer '%s' -> %d", heading.c_str(), items.size(), ans.c_str(), r);
    for (size_t i = 0; i < items.size(); ++i) {
      std::string d = items[i].detail;
      for (char& c : d)
        if (c == '\n') c = '|';
      LOG_I("sysdlg", "  %zu: %s%s / %s / %s", i, items[i].is_new ? "(new) " : "", items[i].title.c_str(), items[i].subtitle.c_str(), d.c_str());
    }
    return r;
  }
  Dlg d;
  d.v.kind = ui::DialogView::List;
  for (char c : heading) d.v.heading += char(std::toupper(static_cast<unsigned char>(c)));  // as the game's own headings
  for (const ListItem& it : items) {
    d.v.rows.push_back(it.subtitle.empty() ? it.title : it.title + "  -  " + it.subtitle);
    d.v.dates.push_back(date_text(it.mtime));
    d.v.details.push_back(it.detail);
  }
  d.v.focus = 0;
  list_move(&d, std::clamp(focus, 0, std::max(int(items.size()) - 1, 0)));
  d.v.button = -1;
  LOG_I("sysdlg", "list '%s' shown (%zu item(s))", heading.c_str(), items.size());
  if (!show(&d)) {
    LOG_W("sysdlg", "list '%s': no window to show it; answered as back", heading.c_str());
    return -1;
  }
  LOG_I("sysdlg", "list '%s': user chose %d", heading.c_str(), d.result);
  return d.result;
}

bool confirm(const std::string& text) {
  std::lock_guard lk(g_mu);
  std::string ans;
  if (scripted(&ans)) {
    LOG_I("sysdlg", "question '%s': scripted answer '%s'", text.c_str(), ans.c_str());
    return ans == "yes";
  }
  Dlg d;
  d.v.kind = ui::DialogView::Question;
  d.v.heading = "SAVED DATA UTILITY";
  d.v.text = text;
  d.v.button = 0;  // YES
  LOG_I("sysdlg", "question '%s' shown", text.c_str());
  if (!show(&d)) {
    LOG_W("sysdlg", "question '%s': no window to show it; answered no", text.c_str());
    return false;
  }
  LOG_I("sysdlg", "question '%s': user answered %s", text.c_str(), d.result ? "yes" : "no");
  return d.result == 1;
}

void message(const std::string& text) {
  std::lock_guard lk(g_mu);
  std::string ans;
  if (scripted(&ans)) {
    LOG_I("sysdlg", "message '%s': scripted answer '%s'", text.c_str(), ans.c_str());
    return;
  }
  Dlg d;
  d.v.kind = ui::DialogView::Message;
  d.v.heading = "SAVED DATA UTILITY";
  d.v.text = text;
  d.v.button = 0;
  LOG_I("sysdlg", "message '%s' shown", text.c_str());
  if (!show(&d)) LOG_W("sysdlg", "message '%s': no window to show it", text.c_str());
  else LOG_I("sysdlg", "message '%s' acknowledged", text.c_str());
}

}  // namespace rt::sysdlg
