#include "runtime/sysdialog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>

#include "runtime/config.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"

namespace rt::sysdlg {
namespace {

std::atomic<int> g_active{0};
std::mutex g_mu;  // one dialog at a time

// ---- scripted answers (--dialog, and live "dialog ..." control commands) ----
bool take_live(std::string*) { return false; }  // player builds: no scripted answers
bool scripted(std::string*) { return false; }

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
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

// ---- controller navigation ----
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
u32 pad_now() {
  XINPUT_STATE xs{};
  XGetState get = xinput();
  if (!get || get(0, &xs) != ERROR_SUCCESS) return 0;
  const auto& g = xs.Gamepad;
  u32 b = 0;
  if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > 16000) b |= P_UP;
  if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -16000) b |= P_DOWN;
  if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16000) b |= P_LEFT;
  if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16000) b |= P_RIGHT;
  if (g.wButtons & XINPUT_GAMEPAD_A) b |= P_ACCEPT;
  if (g.wButtons & XINPUT_GAMEPAD_B) b |= P_BACK;
  if (g.wButtons & XINPUT_GAMEPAD_X) b |= P_X;
  if (g.wButtons & XINPUT_GAMEPAD_Y) b |= P_Y;
  if (g.wButtons & XINPUT_GAMEPAD_START) b |= P_START;
  return b;
}

// ---- the dialog window ----
enum { ID_LIST = 100, ID_TEXT, ID_DETAIL, ID_ACCEPT, ID_BACK, ID_EDIT, ID_KEY0 = 200 };
// Text entry keyboard for controller use (the console's OSK is controller-driven).
const wchar_t kKeys[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.!?'&@#() ";
constexpr int kKeyCols = 12;
struct Dlg {
  enum Kind { List, Question, Message, Text } kind = Message;
  std::wstring heading, text;
  std::vector<std::wstring> rows, details;
  int focus = 0;
  int result = -1;  // List: index or -1; Question: 1 yes / 0 no; Message: 0
  bool done = false;
  bool accept_focused = true;  // Question: which button has the focus
  u32 pad_last = ~0u;           // buttons held when the dialog opened count as released only once
  HWND wnd = nullptr, list = nullptr, detail = nullptr, accept = nullptr, back = nullptr, edit = nullptr;
  HFONT font = nullptr;
  std::wstring edit_text;  // Text: initial value in, entered value out
  u32 max_chars = 0;
  std::vector<HWND> keys;  // Text: the on-screen keyboard
  int key = 0;             // Text: selected key
};
std::atomic<HWND> g_open_wnd{nullptr};

void finish(Dlg* d, int result) {
  d->result = result;
  d->done = true;
}

void update_detail(Dlg* d) {
  if (!d->list || !d->detail) return;
  int i = int(SendMessageW(d->list, LB_GETCURSEL, 0, 0));
  SetWindowTextW(d->detail, i >= 0 && i < int(d->details.size()) ? d->details[i].c_str() : L"");
}

void type_key(Dlg* d, wchar_t c) {
  int n = GetWindowTextLengthW(d->edit);
  if (u32(n) >= d->max_chars) return;
  wchar_t s[2] = {c, 0};
  SendMessageW(d->edit, EM_SETSEL, n, n);
  SendMessageW(d->edit, EM_REPLACESEL, TRUE, LPARAM(s));
}
void backspace(Dlg* d) {
  int n = GetWindowTextLengthW(d->edit);
  if (n <= 0) return;
  SendMessageW(d->edit, EM_SETSEL, n - 1, n);
  SendMessageW(d->edit, EM_REPLACESEL, TRUE, LPARAM(L""));
}

void accept_current(Dlg* d) {
  if (d->kind == Dlg::Text) {
    int n = GetWindowTextLengthW(d->edit);
    std::wstring t(size_t(n) + 1, L'\0');
    GetWindowTextW(d->edit, t.data(), n + 1);
    t.resize(size_t(n));
    d->edit_text = t;
    finish(d, 1);
    return;
  }
  if (d->kind == Dlg::List) {
    int i = int(SendMessageW(d->list, LB_GETCURSEL, 0, 0));
    if (i >= 0) finish(d, i);
  } else if (d->kind == Dlg::Question) {
    finish(d, d->accept_focused ? 1 : 0);
  } else {
    finish(d, 0);
  }
}

LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  Dlg* d = reinterpret_cast<Dlg*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_COMMAND:
      if (!d) break;
      if (LOWORD(wp) == ID_ACCEPT || LOWORD(wp) == IDOK) {
        if (d->kind == Dlg::Question) d->accept_focused = LOWORD(wp) == ID_ACCEPT || GetFocus() != d->back;
        accept_current(d);
      } else if (LOWORD(wp) == ID_BACK || LOWORD(wp) == IDCANCEL) {
        finish(d, d->kind == Dlg::List ? -1 : 0);
      } else if (LOWORD(wp) == ID_LIST && HIWORD(wp) == LBN_SELCHANGE) {
        update_detail(d);
      } else if (LOWORD(wp) == ID_LIST && HIWORD(wp) == LBN_DBLCLK) {
        accept_current(d);
      } else if (LOWORD(wp) >= ID_KEY0 && LOWORD(wp) < ID_KEY0 + int(std::size(kKeys)) - 1) {
        type_key(d, kKeys[LOWORD(wp) - ID_KEY0]);
      }
      return 0;
    case WM_TIMER: {
      if (!d) break;
      if (std::string ans; take_live(&ans)) {  // a live answer for the dialog already on screen
        LOG_I("sysdlg", "open dialog: live answer '%s'", ans.c_str());
        if (d->kind == Dlg::Text) {
          if (ans.rfind("text:", 0) == 0) {
            d->edit_text = widen(ans.substr(5)).substr(0, d->max_chars);
            finish(d, 1);
          } else {
            finish(d, 0);
          }
        } else if (d->kind == Dlg::List) {
          int r = -1;
          if (ans.rfind("choose:text=", 0) == 0) {
            std::wstring want = widen(ans.substr(12));
            for (size_t i = 0; i < d->rows.size() && r < 0; ++i)
              if ((d->rows[i] + L"\n" + d->details[i]).find(want) != std::wstring::npos) r = int(i);
          } else if (ans.rfind("choose:", 0) == 0 && ans != "choose:cancel") {
            r = std::atoi(ans.c_str() + 7);
          }
          finish(d, r >= 0 && r < int(d->rows.size()) ? r : -1);
        } else if (d->kind == Dlg::Question) {
          finish(d, ans == "yes" ? 1 : 0);
        } else {
          finish(d, 0);
        }
        return 0;
      }
      u32 now = pad_now(), edge = now & ~d->pad_last;
      d->pad_last = now;
      if (d->kind == Dlg::List && (edge & (P_UP | P_DOWN))) {
        int n = int(SendMessageW(d->list, LB_GETCOUNT, 0, 0));
        int i = int(SendMessageW(d->list, LB_GETCURSEL, 0, 0));
        i = std::clamp(i + ((edge & P_DOWN) ? 1 : -1), 0, std::max(n - 1, 0));
        SendMessageW(d->list, LB_SETCURSEL, WPARAM(i), 0);
        update_detail(d);
      }
      if (d->kind == Dlg::Question && (edge & (P_LEFT | P_RIGHT))) {
        d->accept_focused = (edge & P_LEFT) != 0;
        SetFocus(d->accept_focused ? d->accept : d->back);
      }
      if (d->kind == Dlg::Text) {
        int n = int(d->keys.size()), rows = (n + kKeyCols - 1) / kKeyCols;
        int r = d->key / kKeyCols, c = d->key % kKeyCols;
        if (edge & P_LEFT) c = (c + kKeyCols - 1) % kKeyCols;
        if (edge & P_RIGHT) c = (c + 1) % kKeyCols;
        if (edge & P_UP) r = (r + rows - 1) % rows;
        if (edge & P_DOWN) r = (r + 1) % rows;
        if (edge & (P_LEFT | P_RIGHT | P_UP | P_DOWN)) {
          d->key = std::min(r * kKeyCols + c, n - 1);
          SetFocus(d->keys[size_t(d->key)]);
        }
        if (edge & P_ACCEPT) type_key(d, kKeys[d->key]);
        if (edge & P_X) backspace(d);
        if (edge & P_Y) type_key(d, L' ');
        if (edge & P_START) accept_current(d);
        if (edge & P_BACK) finish(d, 0);
        return 0;
      }
      if (edge & P_ACCEPT) accept_current(d);
      if (edge & P_BACK) finish(d, d->kind == Dlg::List ? -1 : 0);
      return 0;
    }
    case WM_CLOSE:
      if (d) finish(d, d->kind == Dlg::List ? -1 : 0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// Runs on the window thread: builds the window, loops until answered.
void run_dialog(Dlg* d) {
  HWND owner = static_cast<HWND>(gpu::window_handle());
  HINSTANCE inst = GetModuleHandleW(nullptr);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dlg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"ac4_sysdialog";
    RegisterClassExW(&wc);
    registered = true;
  }
  const int dpi = int(GetDpiForWindow(owner) ? GetDpiForWindow(owner) : 96);
  auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };  // sizes below are at 96 DPI
  const int W = S(560), H = S(d->kind == Dlg::List ? 460 : d->kind == Dlg::Text ? 420 : 220);
  RECT orc{};
  GetWindowRect(owner, &orc);
  RECT r{0, 0, W, H};
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  AdjustWindowRectEx(&r, style, FALSE, ex);
  int x = orc.left + ((orc.right - orc.left) - (r.right - r.left)) / 2, y = orc.top + ((orc.bottom - orc.top) - (r.bottom - r.top)) / 2;
  d->wnd = CreateWindowExW(ex, L"ac4_sysdialog", d->heading.c_str(), style, x, y, r.right - r.left, r.bottom - r.top, owner, nullptr,
                           inst, nullptr);
  if (!d->wnd) {
    finish(d, d->kind == Dlg::List ? -1 : 0);
    return;
  }
  SetWindowLongPtrW(d->wnd, GWLP_USERDATA, LONG_PTR(d));
  d->font = CreateFontW(-S(17), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int cx, int cy, int cw, int ch, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, cx, cy, cw, ch, d->wnd, HMENU(INT_PTR(id)), inst, nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(d->font), TRUE);
    return c;
  };
  const wchar_t* accept_label = d->kind == Dlg::Question ? L"Yes" : L"OK";
  if (d->kind == Dlg::List) {
    d->list = child(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, S(12), S(12), W - S(24), S(300), ID_LIST);
    for (const auto& row : d->rows) SendMessageW(d->list, LB_ADDSTRING, 0, LPARAM(row.c_str()));
    SendMessageW(d->list, LB_SETCURSEL, WPARAM(d->focus), 0);
    d->detail = child(L"STATIC", L"", SS_LEFT, S(12), S(320), W - S(24), S(80), ID_DETAIL);
    update_detail(d);
    d->accept = child(L"BUTTON", accept_label, WS_TABSTOP | BS_DEFPUSHBUTTON, W - S(236), H - S(46), S(108), S(32), ID_ACCEPT);
    d->back = child(L"BUTTON", L"Back", WS_TABSTOP | BS_PUSHBUTTON, W - S(120), H - S(46), S(108), S(32), ID_BACK);
    SetFocus(d->list);
  } else if (d->kind == Dlg::Text) {
    child(L"STATIC", d->text.c_str(), SS_LEFT, S(16), S(16), W - S(32), S(90), ID_TEXT);
    d->edit = child(L"EDIT", d->edit_text.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, S(16), S(116), W - S(32), S(30), ID_EDIT);
    SendMessageW(d->edit, EM_SETLIMITTEXT, WPARAM(d->max_chars), 0);
    SendMessageW(d->edit, EM_SETSEL, 0, -1);
    const int n = int(std::size(kKeys)) - 1, kw = (W - S(32)) / kKeyCols;
    for (int i = 0; i < n; ++i) {
      wchar_t label[8] = {kKeys[i], 0};
      if (kKeys[i] == L' ') wcscpy(label, L"Space");
      d->keys.push_back(child(L"BUTTON", label, WS_TABSTOP | BS_PUSHBUTTON, S(16) + (i % kKeyCols) * kw, S(160) + (i / kKeyCols) * S(36), kw - S(4), S(32),
                              ID_KEY0 + i));
    }
    child(L"STATIC", L"Controller: D-pad select, A type, X delete, Y space, Start OK, B back", SS_LEFT, S(16), H - S(84), W - S(32), S(22),
          ID_TEXT);
    d->accept = child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, W - S(236), H - S(46), S(108), S(32), ID_ACCEPT);
    d->back = child(L"BUTTON", L"Back", WS_TABSTOP | BS_PUSHBUTTON, W - S(120), H - S(46), S(108), S(32), ID_BACK);
    SetFocus(d->edit);
  } else {
    child(L"STATIC", d->text.c_str(), SS_LEFT, S(16), S(16), W - S(32), H - S(80), ID_TEXT);
    d->accept = child(L"BUTTON", accept_label, WS_TABSTOP | BS_DEFPUSHBUTTON, W - (d->kind == Dlg::Question ? S(236) : S(120)), H - S(46), S(108), S(32),
                      ID_ACCEPT);
    if (d->kind == Dlg::Question) d->back = child(L"BUTTON", L"No", WS_TABSTOP | BS_PUSHBUTTON, W - S(120), H - S(46), S(108), S(32), ID_BACK);
    SetFocus(d->accept);
  }
  d->pad_last = pad_now();
  g_open_wnd = d->wnd;
  SetTimer(d->wnd, 1, 30, nullptr);
  EnableWindow(owner, FALSE);
  ShowWindow(d->wnd, SW_SHOW);
  SetForegroundWindow(d->wnd);
  MSG m;
  while (!d->done && GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(d->wnd, &m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  KillTimer(d->wnd, 1);
  g_open_wnd = nullptr;
  EnableWindow(owner, TRUE);
  DestroyWindow(d->wnd);
  DeleteObject(d->font);
  SetForegroundWindow(owner);
}

// Shows the dialog interactively; false if there is no window to show it on.
bool show(Dlg* d) {
  struct Hold {
    Hold() { g_active.fetch_add(1); }
    ~Hold() { g_active.fetch_sub(1); }
  } hold;
  return gpu::run_on_window_thread([d] { run_dialog(d); });
}

}  // namespace

bool active() { return g_active.load() > 0; }

void abort() {
  if (HWND w = g_open_wnd.load()) PostMessageW(w, WM_CLOSE, 0, 0);
}

bool text_input(const std::string& prompt, const std::u16string& initial, u32 max_chars, std::u16string* out) {
  std::lock_guard lk(g_mu);
  std::string ans;
  if (scripted(&ans)) {
    bool ok = ans.rfind("text:", 0) == 0;
    if (ok) {
      std::string t = ans.substr(5);
      std::wstring w = widen(t);
      out->assign(w.begin(), w.end());
      if (out->size() > max_chars) out->resize(max_chars);
    }
    LOG_I("sysdlg", "text input '%s': scripted answer '%s'", prompt.c_str(), ans.c_str());
    return ok;
  }
  Dlg d;
  d.kind = Dlg::Text;
  d.heading = L"Text Input";
  d.text = widen(prompt);
  d.edit_text.assign(initial.begin(), initial.end());
  d.max_chars = max_chars;
  if (!show(&d)) {
    LOG_W("sysdlg", "text input '%s': no window to show it; answered as back", prompt.c_str());
    return false;
  }
  if (d.result != 1) {
    LOG_I("sysdlg", "text input '%s': user backed out", prompt.c_str());
    return false;
  }
  out->assign(d.edit_text.begin(), d.edit_text.end());
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
  d.kind = Dlg::List;
  d.heading = widen(heading);
  for (const ListItem& it : items) {
    std::string row = it.title;
    if (!it.subtitle.empty()) row += "  -  " + it.subtitle;
    if (it.mtime) row += "    " + date_text(it.mtime);
    d.rows.push_back(widen(row));
    d.details.push_back(widen(it.detail));
  }
  d.focus = std::clamp(focus, 0, std::max(int(items.size()) - 1, 0));
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
  d.kind = Dlg::Question;
  d.heading = L"Saved Data Utility";
  d.text = widen(text);
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
  d.kind = Dlg::Message;
  d.heading = L"Saved Data Utility";
  d.text = widen(text);
  if (!show(&d)) LOG_W("sysdlg", "message '%s': no window to show it", text.c_str());
  else LOG_I("sysdlg", "message '%s' acknowledged", text.c_str());
}

}  // namespace rt::sysdlg
