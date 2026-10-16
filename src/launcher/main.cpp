// AC4-Launcher: the player's way in. Takes the game's folder and the folder with the files the
// player decrypted (checked by SHA-256; the port decrypts nothing), or prepares those files with
// the player's own RPCS3 in one step (rpcs3.h: a private copy of RPCS3 does it), runs the first-time setup
// (setup.h), offers the display mode, resolution and frame rate (the shared settings file, also
// changed in game under SYSTEM > PC SETTINGS) and starts the game. The More menu holds the rest:
// render scale, monitor, presets, saves (open, back up, restore), safe mode, settings reset, the
// install folder, rebuild and the log. Settings and saves are the player's own: they live in
// Saved Games\ARMORED CORE 4, the install folder holds the rest.
// Contains no game data or game-derived code.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "common/bytes.h"
#include "common/ini.h"
#include "common/settings_file.h"
#include "launcher/inputs.h"
#include "launcher/process.h"
#include "launcher/rpcs3.h"
#include "launcher/setup.h"

namespace fs = std::filesystem;
using namespace launcher;

#define AC4_WIDEN2(x) L##x
#define AC4_WIDEN(x) AC4_WIDEN2(x)
#define AC4_VERSION_W AC4_WIDEN(AC4_VERSION)

namespace {

constexpr UINT WM_SETUP = WM_APP + 1, WM_CHECKED = WM_APP + 2, WM_GAME_EXIT = WM_APP + 3, WM_PREP = WM_APP + 4;
constexpr const wchar_t* kTitle = L"ARMORED CORE 4";
constexpr const wchar_t* kSavePrefix = L"BLUS30027";  // the game's save folders
enum Id {
  kDiscEdit = 100, kFilesEdit,
  kDiscBrowse = 110, kFilesBrowse, kPrepFiles,
  kDiscState = 120, kFilesState,
  kPrepare = 140, kCancel, kProgress = 143, kStatus,
  kFps = 150, kMode, kResolution,
  kMore = 160,
  kPlay = 170, kPlayState,
  kLabel = 200,
  // the More menu
  kMenuScale = 300,    // + index into g_scale_items
  kMenuMonitor = 320,  // + index into g_monitor_items
  kMenuPreset = 340,   // + index into kPresets
  kMenuSavesOpen = 360, kMenuSavesBackup, kMenuSavesRestore,
  kMenuSafePlay = 370, kMenuResetSettings,
  kMenuInstall = 380, kMenuRebuild, kMenuOpenLog, kMenuWhichFiles,
};

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font = nullptr, g_big = nullptr;
UINT g_dpi = 96;
int S(int v) { return MulDiv(v, int(g_dpi), 96); }

fs::path g_src, g_ini_path;
fs::path g_user_override;  // launcher.ini [paths] user
SetupConfig g_cfg;
Setup g_setup;
Rpcs3Prep g_prep;
fs::path g_rpcs3, g_pup;  // the player's rpcs3.exe and PS3UPDAT.PUP (Prepare files)
HANDLE g_game = nullptr;
bool g_ready = false;
std::wstring g_note;  // a message for the status line (a setup or game error) until the next action

std::mutex g_check_mu;
Status g_disc_check, g_files_check;
bool g_checked = false;
std::atomic<int> g_check_gen{0};

HWND item(int id) { return GetDlgItem(g_wnd, id); }

std::wstring get_text(int id) {
  int n = GetWindowTextLengthW(item(id));
  std::wstring s(size_t(n) + 1, L'\0');
  GetWindowTextW(item(id), s.data(), n + 1);
  s.resize(size_t(n));
  return s;
}
void set_text(int id, const std::wstring& s) { SetWindowTextW(item(id), s.c_str()); }
void show(int id, bool on) { ShowWindow(item(id), on ? SW_SHOWNA : SW_HIDE); }

// ---- launcher.ini: where the player's files and the install folder are ----
void load_paths() {
  ac4::Ini ini = ac4::Ini::load(g_ini_path);
  auto p = [&](const char* k) { return fs::path(widen(ini.get_or("paths", k, ""))); };
  g_cfg.disc = p("disc");
  g_cfg.files = p("files");
  g_cfg.data = p("install");
  g_rpcs3 = p("rpcs3");
  g_pup = p("pup");
  g_user_override = p("user");  // optional: settings and saves somewhere else than Saved Games
}

void save_paths() {
  try {
    ac4::Ini ini = ac4::Ini::load(g_ini_path);
    if (ini.empty()) ini = ac4::Ini::parse("# ARMORED CORE 4 launcher: the locations chosen in the launcher\n");
    ini.set("paths", "disc", narrow(g_cfg.disc.wstring()));
    ini.set("paths", "files", narrow(g_cfg.files.wstring()));
    ini.set("paths", "install", narrow(g_cfg.data.wstring()));
    ini.set("paths", "rpcs3", narrow(g_rpcs3.wstring()));
    ini.set("paths", "pup", narrow(g_pup.wstring()));
    std::error_code ec;
    fs::create_directories(g_ini_path.parent_path(), ec);
    ini.save(g_ini_path);
  } catch (const std::exception&) {
  }
}

bool writable(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path probe = dir / L".write-test";
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

fs::path local_appdata() {
  PWSTR p = nullptr;
  fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r / L"AC4 Port";
}

// The player's own folder: settings.ini, savedata\ and backups\ (Saved Games\ARMORED CORE 4;
// under Local AppData when Windows has no Saved Games folder; launcher.ini [paths] user names
// another folder).
fs::path user_dir() {
  if (!g_user_override.empty()) return g_user_override;
  PWSTR p = nullptr;
  fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, KF_FLAG_CREATE, nullptr, &p))) r = fs::path(p) / L"ARMORED CORE 4";
  CoTaskMemFree(p);
  return r.empty() ? local_appdata() / L"user" : r;
}

// The port's sources: the launcher sits in their folder (a release) or in a build folder below.
fs::path find_sources() {
  wchar_t buf[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  fs::path dir = fs::path(buf).parent_path();
  for (int i = 0; i < 5 && !dir.empty(); ++i, dir = dir.parent_path()) {
    std::error_code ec;
    if (fs::exists(dir / "CMakeLists.txt", ec) && fs::exists(dir / "data" / "toolchain.txt", ec) && fs::exists(dir / "src" / "runtime", ec))
      return dir;
    if (dir == dir.parent_path()) break;
  }
  return {};
}

// ---- settings (the shared file in the player's folder) ----
fs::path settings_path() { return user_dir() / "settings.ini"; }

ac4::settings::Display read_display() {
  try {
    return ac4::settings::read_display(ac4::settings::load_or_create(settings_path(), nullptr));
  } catch (const std::exception&) {
    return {};
  }
}

// Changes settings.ini (a message box when it cannot be written).
template <class F>
bool edit_settings(F&& change) {
  try {
    ac4::Ini ini = ac4::settings::load_or_create(settings_path(), nullptr);
    change(ini);
    ini.save(settings_path());
    return true;
  } catch (const std::exception& e) {
    MessageBoxW(g_wnd, widen(std::string("Settings not saved: ") + e.what()).c_str(), kTitle, MB_ICONWARNING);
    return false;
  }
}

// The monitors (Windows' order) and the one the settings choose ("" or unknown: the primary).
struct MonitorItem {
  std::wstring device;
  RECT rect{}, work{};
  bool primary = false;
};
std::vector<MonitorItem> monitors() {
  std::vector<MonitorItem> out;
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(hm, &mi))
      reinterpret_cast<std::vector<MonitorItem>*>(lp)->push_back({mi.szDevice, mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
  }, reinterpret_cast<LPARAM>(&out));
  return out;
}
std::vector<MonitorItem> g_monitor_items;  // the monitor menu's entries
MonitorItem chosen_monitor(const ac4::settings::Display& d) {
  std::vector<MonitorItem> ms = monitors();
  MonitorItem pick;
  for (const MonitorItem& m : ms)
    if (m.primary) pick = m;
  for (const MonitorItem& m : ms)
    if (!d.monitor.empty() && narrow(m.device) == d.monitor) pick = m;
  return pick;
}

struct ModeItem {
  int w, h, hz;
};
std::vector<ModeItem> g_res_items;  // the resolution combo's entries for the current mode
std::vector<int> g_scale_items;     // the render scale menu's percentages

void monitor_size(const MonitorItem& mon, int* w, int* h, int* hz) {
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsW(mon.device.empty() ? nullptr : mon.device.c_str(), ENUM_CURRENT_SETTINGS, &dm);
  *w = int(dm.dmPelsWidth);
  *h = int(dm.dmPelsHeight);
  *hz = int(dm.dmDisplayFrequency);
}

void fill_resolutions(const ac4::settings::Display& d) {
  HWND cb = item(kResolution);
  SendMessageW(cb, CB_RESETCONTENT, 0, 0);
  g_res_items.clear();
  MonitorItem mon = chosen_monitor(d);
  int mw, mh, mhz;
  monitor_size(mon, &mw, &mh, &mhz);
  int sel = 0;
  auto add = [&](const std::wstring& text, ModeItem m) {
    SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(text.c_str()));
    g_res_items.push_back(m);
  };
  if (d.mode == ac4::settings::Mode::Windowed) {
    RECT wa = mon.work;
    for (const auto& s : ac4::settings::window_sizes()) {
      if ((s.w > wa.right - wa.left || s.h > wa.bottom - wa.top) && !(s.w == d.width && s.h == d.height)) continue;
      if (s.w == d.width && s.h == d.height) sel = int(g_res_items.size());
      add(std::to_wstring(s.w) + L" x " + std::to_wstring(s.h), {s.w, s.h, 0});
    }
    if (std::none_of(g_res_items.begin(), g_res_items.end(), [&](const ModeItem& m) { return m.w == d.width && m.h == d.height; })) {
      sel = int(g_res_items.size());
      add(std::to_wstring(d.width) + L" x " + std::to_wstring(d.height), {d.width, d.height, 0});
    }
  } else if (d.mode == ac4::settings::Mode::Fullscreen) {
    add(L"Desktop (" + std::to_wstring(mw) + L" x " + std::to_wstring(mh) + L", " + std::to_wstring(mhz) + L" Hz)", {0, 0, 0});
    std::set<std::tuple<int, int, int>> seen;
    std::vector<ModeItem> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(mon.device.empty() ? nullptr : mon.device.c_str(), i, &dm); ++i)
      if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 640 && seen.insert({int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)}).second)
        modes.push_back({int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)});
    std::sort(modes.begin(), modes.end(), [](const ModeItem& a, const ModeItem& b) {
      return a.w != b.w ? a.w > b.w : a.h != b.h ? a.h > b.h : a.hz > b.hz;
    });
    for (const ModeItem& m : modes) {
      if (m.w == d.fs_width && m.h == d.fs_height && (m.hz == d.fs_hz || !d.fs_hz)) sel = int(g_res_items.size());
      add(std::to_wstring(m.w) + L" x " + std::to_wstring(m.h) + L", " + std::to_wstring(m.hz) + L" Hz", m);
    }
  } else {
    add(std::to_wstring(mw) + L" x " + std::to_wstring(mh), {mw, mh, 0});
  }
  SendMessageW(cb, CB_SETCURSEL, WPARAM(sel), 0);
}

void load_settings_controls() {
  if (g_cfg.data.empty()) return;
  ac4::settings::Display d = read_display();
  HWND fps = item(kFps);
  SendMessageW(fps, CB_RESETCONTENT, 0, 0);
  int sel = -1;
  const auto& choices = ac4::settings::fps_choices();
  for (size_t i = 0; i < choices.size(); ++i) {
    std::wstring t = widen(choices[i].label);
    SendMessageW(fps, CB_ADDSTRING, 0, LPARAM(t.c_str()));
    if (std::abs(choices[i].fps - d.fps) < 0.01) sel = int(i);
  }
  if (sel < 0) {
    std::wstring t = widen(ac4::settings::fps_text(d.fps)) + L" (untested)";
    SendMessageW(fps, CB_ADDSTRING, 0, LPARAM(t.c_str()));
    sel = int(choices.size());
  }
  SendMessageW(fps, CB_SETCURSEL, WPARAM(sel), 0);
  HWND mode = item(kMode);
  SendMessageW(mode, CB_RESETCONTENT, 0, 0);
  for (const wchar_t* m : {L"Windowed", L"Fullscreen", L"Borderless fullscreen"}) SendMessageW(mode, CB_ADDSTRING, 0, LPARAM(m));
  SendMessageW(mode, CB_SETCURSEL, d.mode == ac4::settings::Mode::Fullscreen ? 1 : d.mode == ac4::settings::Mode::Borderless ? 2 : 0, 0);
  fill_resolutions(d);
}

void save_setting(int id) {
  if (g_cfg.data.empty()) return;
  int sel = int(SendMessageW(item(id), CB_GETCURSEL, 0, 0));
  ac4::settings::Display d = read_display();
  edit_settings([&](ac4::Ini& ini) {
    if (id == kFps) {
      const auto& choices = ac4::settings::fps_choices();
      if (sel >= 0 && sel < int(choices.size())) ini.set("display", "fps", ac4::settings::fps_text(choices[size_t(sel)].fps));
    } else if (id == kMode) {
      d.mode = sel == 1 ? ac4::settings::Mode::Fullscreen : sel == 2 ? ac4::settings::Mode::Borderless : ac4::settings::Mode::Windowed;
      ini.set("display", "mode", ac4::settings::mode_name(d.mode));
    } else if (id == kResolution && sel >= 0 && sel < int(g_res_items.size())) {
      const ModeItem& m = g_res_items[size_t(sel)];
      if (d.mode == ac4::settings::Mode::Windowed) {
        ini.set("display", "window", ac4::strprintf("%dx%d", m.w, m.h));
      } else if (d.mode == ac4::settings::Mode::Fullscreen) {
        d.fs_width = m.w;
        d.fs_height = m.h;
        d.fs_hz = m.hz;
        ini.set("display", "fullscreen_mode", ac4::settings::fullscreen_text(d));
      }
    }
  });
  if (id == kMode) fill_resolutions(d);
}

// Presets for the display settings (the frame rate in missions and the render scale): what a
// modest PC or a handheld runs well (estimates; docs/STATUS.md lists what was measured).
struct Preset {
  const wchar_t* name;
  const char* fps;
  int render_scale;
};
const Preset kPresets[] = {
    {L"Standard: 60 fps, 100 %", "60", 100},
    {L"Modest PC: 60 fps, 75 %", "60", 75},
    {L"Handheld / low power: 30 fps, 75 %", "30", 75},
    {L"High refresh: 120 fps, 100 %", "120", 100},
};

// ---- saves ----
fs::path saves_dir() { return user_dir() / "savedata"; }
fs::path backups_dir() { return user_dir() / "backups"; }

std::wstring stamp_now() {
  std::time_t t = std::time(nullptr);
  wchar_t b[32];
  std::wcsftime(b, std::size(b), L"%Y%m%d-%H%M%S", std::localtime(&t));
  return b;
}

// Copies the game's save folders (BLUS30027...) from one savedata folder to another.
size_t copy_saves(const fs::path& from, const fs::path& to) {
  size_t n = 0;
  std::error_code ec;
  fs::create_directories(to, ec);
  for (const auto& e : fs::directory_iterator(from, ec)) {
    if (!e.is_directory() || e.path().filename().wstring().rfind(kSavePrefix, 0) != 0) continue;
    fs::copy(e.path(), to / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!ec) ++n;
  }
  return n;
}

// Earlier versions kept settings and saves in the install folder: copied to the player's folder
// once (the originals stay where they are).
void move_to_user_dir() {
  if (g_cfg.data.empty()) return;
  std::error_code ec;
  fs::create_directories(user_dir(), ec);
  fs::path old_settings = ac4::settings::file_in(g_cfg.data);
  if (!fs::exists(settings_path(), ec) && fs::exists(old_settings, ec)) fs::copy_file(old_settings, settings_path(), ec);
  fs::path old_saves = g_cfg.data / "hdd0" / "home" / "00000001" / "savedata";
  bool have = false;
  for (const auto& e : fs::directory_iterator(saves_dir(), ec))
    have |= e.is_directory() && e.path().filename().wstring().rfind(kSavePrefix, 0) == 0;
  if (!have && fs::exists(old_saves, ec)) copy_saves(old_saves, saves_dir());
  fs::path old_backups = g_cfg.data / "backups";
  if (!fs::exists(backups_dir(), ec) && fs::exists(old_backups, ec)) fs::copy(old_backups, backups_dir(), fs::copy_options::recursive, ec);
}

// ---- checks of the chosen files (background: hashing takes a moment) ----
void start_checks() {
  int gen = ++g_check_gen;
  SetupConfig c = g_cfg;
  std::thread([c, gen] {
    Found f = find_files(c.disc, c.files);
    Status d = check_disc(c.disc, f), m = check_modules(c.files, f);
    {
      std::lock_guard lk(g_check_mu);
      if (gen != g_check_gen) return;
      g_disc_check = d;
      g_files_check = m;
      g_checked = true;
    }
    PostMessageW(g_wnd, WM_CHECKED, WPARAM(gen), 0);
  }).detach();
}

// The install folder must not lie inside a folder holding the game files (nothing is written there).
bool inside(const fs::path& child, const fs::path& parent) {
  if (child.empty() || parent.empty()) return false;
  std::error_code ec;
  std::wstring c = fs::weakly_canonical(fs::absolute(child, ec), ec).wstring(), p = fs::weakly_canonical(fs::absolute(parent, ec), ec).wstring();
  for (auto* s : {&c, &p}) {
    for (auto& ch : *s) ch = wchar_t(towlower(ch));
    while (s->size() > 3 && (s->back() == L'\\' || s->back() == L'/')) s->pop_back();
  }
  return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && (c[p.size()] == L'\\' || c[p.size()] == L'/'));
}
bool data_ok() {
  if (g_cfg.data.empty()) return false;
  for (const fs::path& in : {g_cfg.disc, g_cfg.files})
    if (inside(g_cfg.data, in)) return false;
  return true;
}

bool inputs_ok() {
  std::lock_guard lk(g_check_mu);
  return g_disc_check.verdict == Verdict::Ok && g_files_check.verdict == Verdict::Ok;
}

// Decrypted files are missing or wrong: the window offers to prepare them with RPCS3.
bool files_missing() {
  std::lock_guard lk(g_check_mu);
  if (!g_checked) return false;
  bool eboot = g_disc_check.verdict == Verdict::Bad && g_disc_check.text.find("EBOOT") != std::string::npos;
  return eboot || g_files_check.verdict != Verdict::Ok;
}

// What the status line says when setup is not running.
std::wstring idle_status() {
  if (!g_note.empty()) return g_note;
  if (g_ready) return L"Ready to play.";
  if (!data_ok()) return L"Choose an install folder outside the game's folders (More > Install folder).";
  fs::path p = g_cfg.data;
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
  ULARGE_INTEGER avail{};
  GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr);
  double need = double(Setup::space_needed(g_cfg)) / (1ull << 30), free = double(avail.QuadPart) / (1ull << 30);
  std::wstring where = widen(ac4::strprintf("%.1f GB in ", need)) + g_cfg.data.wstring() + widen(ac4::strprintf(" (%.0f GB free)", free));
  if (free < need) return L"Not enough space: setup needs " + where + L". More > Install folder chooses another place.";
  if (files_missing())
    return L"Prepare files makes them with your RPCS3 (rpcs3.net) and the PS3 update file PS3UPDAT.PUP 4.93 (playstation.com).";
  if (!inputs_ok()) return L"Setup starts when the files above are found: about " + where + L".";
  return L"Set up prepares and builds the game once, a few minutes: " + where + L".";
}

void update_buttons() {
  bool busy = g_setup.running() || g_prep.running(), playing = g_game != nullptr;
  g_ready = !g_cfg.data.empty() && Setup::ready(g_cfg);
  bool prep = !busy && files_missing();
  show(kPrepFiles, prep);
  EnableWindow(item(kPrepFiles), !playing);
  show(kPrepare, !busy && !g_ready && !prep);
  show(kCancel, busy);
  show(kProgress, g_setup.running());
  EnableWindow(item(kPrepare), !playing && inputs_ok() && data_ok());
  EnableWindow(item(kPlay), !busy && !playing && g_ready && inputs_ok());
  for (int id : {kDiscBrowse, kFilesBrowse, kDiscEdit, kFilesEdit}) EnableWindow(item(id), !busy && !playing);
  for (int id : {kFps, kMode, kResolution}) EnableWindow(item(id), !playing && !g_cfg.data.empty());
  if (ac4::settings::Mode m = read_display().mode; m == ac4::settings::Mode::Borderless) EnableWindow(item(kResolution), FALSE);
  if (!busy) set_text(kStatus, idle_status());
  set_text(kPlayState, playing ? L"Playing." : g_ready && inputs_ok() && !busy ? L"Ready." : L"");
}

void refresh_paths_from_ui() {
  g_cfg.disc = get_text(kDiscEdit);
  g_cfg.files = get_text(kFilesEdit);
  save_paths();
  move_to_user_dir();
  set_text(kDiscState, L"Checking...");
  set_text(kFilesState, L"");
  start_checks();
  update_buttons();
}

// ---- file and folder pickers ----
fs::path pick(bool folder, const wchar_t* title, const fs::path& start, const wchar_t* filter_name = nullptr,
              const wchar_t* filter = nullptr) {
  IFileOpenDialog* dlg = nullptr;
  fs::path result;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  dlg->SetTitle(title);
  if (!folder && filter) {
    COMDLG_FILTERSPEC spec[2] = {{filter_name, filter}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, spec);
  }
  std::error_code ec;
  fs::path dir = start;
  while (!dir.empty() && !fs::is_directory(dir, ec)) dir = dir.parent_path();
  IShellItem* si = nullptr;
  if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
    dlg->SetFolder(si);
    si->Release();
  }
  if (SUCCEEDED(dlg->Show(g_wnd))) {
    IShellItem* r = nullptr;
    if (SUCCEEDED(dlg->GetResult(&r))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) result = p;
      CoTaskMemFree(p);
      r->Release();
    }
  }
  dlg->Release();
  return result;
}
fs::path pick_folder(const wchar_t* title, const fs::path& start) { return pick(true, title, start); }

fs::path downloads_dir() {
  PWSTR p = nullptr;
  fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

// ---- Prepare files: the player's RPCS3 makes the decrypted files (rpcs3.h) ----
void prepare_files() {
  std::error_code ec;
  if (!fs::is_regular_file(g_cfg.disc / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", ec)) {
    MessageBoxW(g_wnd, L"Choose the game's folder first (the one with PS3_GAME).", kTitle, MB_ICONINFORMATION);
    return;
  }
  fs::path exe = g_rpcs3;
  if (!fs::is_regular_file(exe, ec)) {
    exe = pick(false, L"Your RPCS3 (rpcs3.exe; get it from rpcs3.net): it prepares the files once", downloads_dir(), L"RPCS3", L"rpcs3.exe");
    if (exe.empty()) return;
  }
  fs::path pup;
  if (rpcs3_system_software(exe) != "4.93") {  // else RPCS3 has the system modules already
    pup = g_pup;
    if (pup_version(pup) != "4.93") {
      pup = pick(false, L"PS3 system software 4.93 update file (PS3UPDAT.PUP, from playstation.com)", downloads_dir(), L"PS3 system software update",
                 L"*.PUP");
      if (pup.empty()) return;
      std::string v = pup_version(pup);
      if (v != "4.93") {
        MessageBoxW(g_wnd,
                    v.empty() ? L"This is not a PS3 system software update file (PS3UPDAT.PUP)."
                              : (L"This update file is PS3 system software " + widen(v) + L"; the port needs 4.93 (playstation.com).").c_str(),
                    kTitle, MB_ICONWARNING);
        return;
      }
    }
  }
  g_rpcs3 = exe;
  if (!pup.empty()) g_pup = pup;
  save_paths();
  Rpcs3Job job;
  job.rpcs3_exe = exe;
  job.pup = pup;
  job.disc = g_cfg.disc;
  job.work = g_cfg.data / "rpcs3-work";
  job.out = g_cfg.data / "decrypted";
  g_note.clear();
  g_prep.start(job, [] { PostMessageW(g_wnd, WM_PREP, 0, 0); });
  update_buttons();
}

void start_checks();
void refresh_paths_from_ui();

// An RPCS3 window waiting for the player during Prepare files: brought to the front once, or
// flashing in the taskbar when Windows keeps the focus with another program.
HWND g_prep_asked = nullptr;
void bring_forward(HWND w) {
  if (!IsWindow(w)) return;
  if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
  if (SetForegroundWindow(w)) return;
  FLASHWINFO fi{sizeof fi, w, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
  FlashWindowEx(&fi);
}

void on_prep_update() {
  Rpcs3Prep::View v = g_prep.view();
  if (HWND(v.ask) != g_prep_asked) {
    g_prep_asked = HWND(v.ask);
    if (g_prep_asked) bring_forward(g_prep_asked);
  }
  if (!v.finished) {
    set_text(kStatus, widen(v.status));
    return;
  }
  if (v.ok) {
    set_text(kFilesEdit, (g_cfg.data / "decrypted").wstring());
    g_note = widen(v.note);
    refresh_paths_from_ui();
  } else {
    g_note = widen(v.error);
    update_buttons();
  }
}

// ---- the game ----
void play(bool safe_mode) {
  fs::path exe = Setup::game_exe(g_cfg);
  std::wstring cmd = quote_arg(exe.wstring()) + L" --data " + quote_arg(g_cfg.data.wstring()) + L" --user " + quote_arg(user_dir().wstring()) +
                     L" --disc " + quote_arg(g_cfg.disc.wstring()) + L" --flash " + quote_arg(Setup::flash_dir(g_cfg).wstring()) + L" --keep-runs 10";
  if (safe_mode) cmd += L" --safe-mode";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  // Only the game's window: no console (the run log holds every message).
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_cfg.data.c_str(), &si, &pi)) {
    MessageBoxW(g_wnd, L"The game could not be started.", kTitle, MB_ICONERROR);
    return;
  }
  CloseHandle(pi.hThread);
  AllowSetForegroundWindow(pi.dwProcessId);  // the game window takes the focus from the launcher
  g_game = pi.hProcess;
  g_note.clear();
  update_buttons();
  ShowWindow(g_wnd, SW_MINIMIZE);
  std::thread([h = pi.hProcess] {
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    PostMessageW(g_wnd, WM_GAME_EXIT, WPARAM(code), 0);
  }).detach();
}

fs::path newest_run_log() {
  fs::path best;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(g_cfg.data / "runs", ec))
    if (e.is_directory() && (best.empty() || e.path().filename() > best.filename())) best = e.path();
  return best.empty() ? best : best / "ac4.log";
}

// ---- layout (96-DPI units) ----
struct Place {
  int id;
  int x, y, w, h;
};
const Place kPlaces[] = {
    {kDiscEdit, 124, 14, 324, 24},   {kDiscBrowse, 454, 13, 90, 26},   {kDiscState, 124, 41, 420, 18},
    {kFilesEdit, 124, 68, 324, 24},  {kFilesBrowse, 454, 67, 90, 26},  {kFilesState, 124, 95, 420, 18},
    {kPrepare, 16, 124, 98, 28},     {kCancel, 16, 124, 98, 28},       {kPrepFiles, 16, 124, 98, 28},
    {kStatus, 124, 124, 420, 34},    {kProgress, 124, 162, 420, 10},
    {kMode, 124, 186, 190, 300},     {kResolution, 320, 186, 224, 300}, {kFps, 124, 220, 190, 300},
    {kMore, 16, 266, 98, 30},        {kPlayState, 124, 272, 200, 20},  {kPlay, 334, 254, 210, 50},
};
struct Label {
  int x, y, w, h;
  const wchar_t* text;
};
const Label kLabels[] = {
    {16, 17, 104, 20, L"Game folder"},
    {16, 71, 104, 20, L"Decrypted files"},
    {16, 189, 104, 20, L"Display"},
    {16, 223, 104, 20, L"Frame rate"},
};
constexpr int kClientW = 560, kClientH = 318;

void layout() {
  for (const Place& p : kPlaces) SetWindowPos(item(p.id), nullptr, S(p.x), S(p.y), S(p.w), S(p.h), SWP_NOZORDER | SWP_NOACTIVATE);
  for (size_t i = 0; i < std::size(kLabels); ++i) {
    const Label& l = kLabels[i];
    SetWindowPos(item(kLabel + int(i)), nullptr, S(l.x), S(l.y), S(l.w), S(l.h), SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void make_fonts() {
  if (g_font) DeleteObject(g_font);
  if (g_big) DeleteObject(g_big);
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof ncm;
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi);
  g_font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW big = ncm.lfMessageFont;
  big.lfHeight = -MulDiv(16, int(g_dpi), 72);
  big.lfWeight = FW_SEMIBOLD;
  g_big = CreateFontIndirectW(&big);
  EnumChildWindows(g_wnd, [](HWND h, LPARAM) -> BOOL {
    SendMessageW(h, WM_SETFONT, WPARAM(GetDlgCtrlID(h) == kPlay ? g_big : g_font), TRUE);
    return TRUE;
  }, 0);
}

void create_controls() {
  auto make = [](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, HMENU(INT_PTR(id)), g_inst, nullptr);
  };
  for (size_t i = 0; i < std::size(kLabels); ++i) make(L"STATIC", kLabels[i].text, SS_LEFT, kLabel + int(i));
  for (int i = 0; i < 2; ++i) {
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kDiscEdit + i, WS_EX_CLIENTEDGE);
    make(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, kDiscBrowse + i);
    make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kDiscState + i);
  }
  make(L"BUTTON", L"Prepare files", WS_TABSTOP | BS_PUSHBUTTON, kPrepFiles);
  make(L"BUTTON", L"Set up", WS_TABSTOP | BS_PUSHBUTTON, kPrepare);
  make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, kCancel);
  make(L"STATIC", L"", SS_LEFT, kStatus);
  make(PROGRESS_CLASSW, L"", PBS_SMOOTH, kProgress);
  SendMessageW(item(kProgress), PBM_SETRANGE32, 0, 1000);
  for (int id : {kMode, kResolution, kFps}) make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
  make(L"BUTTON", L"More...", WS_TABSTOP | BS_PUSHBUTTON, kMore);
  make(L"STATIC", L"", SS_RIGHT, kPlayState);
  make(L"BUTTON", L"PLAY", WS_TABSTOP | BS_DEFPUSHBUTTON, kPlay);
  show(kPrepFiles, false);
  show(kCancel, false);
  show(kProgress, false);
}

// The More menu: everything beyond the bare necessities.
void more_menu() {
  bool playing = g_game != nullptr, busy = g_setup.running(), data = !g_cfg.data.empty();
  ac4::settings::Display d = read_display();
  HMENU scale = CreatePopupMenu();
  g_scale_items.clear();
  for (int pct = ac4::settings::kRenderScaleMin; pct <= ac4::settings::kRenderScaleMax; pct += 25) g_scale_items.push_back(pct);
  if (std::find(g_scale_items.begin(), g_scale_items.end(), d.render_scale) == g_scale_items.end()) {
    g_scale_items.push_back(d.render_scale);
    std::sort(g_scale_items.begin(), g_scale_items.end());
  }
  for (size_t i = 0; i < g_scale_items.size(); ++i) {
    int pct = g_scale_items[i];
    std::wstring t = std::to_wstring(pct) + L" %" + (pct == 100 ? L" (the output resolution)" : L"");
    AppendMenuW(scale, MF_STRING | (pct == d.render_scale ? MF_CHECKED : 0), kMenuScale + i, t.c_str());
  }
  HMENU presets = CreatePopupMenu();
  for (size_t i = 0; i < std::size(kPresets); ++i) AppendMenuW(presets, MF_STRING, kMenuPreset + i, kPresets[i].name);
  HMENU m = CreatePopupMenu();
  UINT set_flags = MF_POPUP | (data && !playing ? 0 : MF_GRAYED);
  AppendMenuW(m, set_flags, UINT_PTR(scale), L"Render scale");
  g_monitor_items = monitors();
  if (g_monitor_items.size() > 1) {
    HMENU mon = CreatePopupMenu();
    MonitorItem cur = chosen_monitor(d);
    for (size_t i = 0; i < g_monitor_items.size(); ++i) {
      const MonitorItem& mi = g_monitor_items[i];
      std::wstring t = std::to_wstring(i + 1) + L": " + std::to_wstring(mi.rect.right - mi.rect.left) + L" x " +
                       std::to_wstring(mi.rect.bottom - mi.rect.top) + (mi.primary ? L" (primary)" : L"");
      AppendMenuW(mon, MF_STRING | (mi.device == cur.device ? MF_CHECKED : 0), kMenuMonitor + i, t.c_str());
    }
    AppendMenuW(m, set_flags, UINT_PTR(mon), L"Monitor");
  }
  AppendMenuW(m, set_flags, UINT_PTR(presets), L"Presets");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  UINT idle = data && !playing ? 0 : MF_GRAYED;
  AppendMenuW(m, MF_STRING | (data ? 0 : MF_GRAYED), kMenuSavesOpen, L"Open the saves folder");
  AppendMenuW(m, MF_STRING | idle, kMenuSavesBackup, L"Back up the saves");
  AppendMenuW(m, MF_STRING | idle, kMenuSavesRestore, L"Restore saves...");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (!busy && !playing && g_ready && inputs_ok() ? 0 : MF_GRAYED), kMenuSafePlay,
              L"Play in safe mode (default display settings, this start only)");
  AppendMenuW(m, MF_STRING | idle, kMenuResetSettings, L"Reset all settings...");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (!busy && !playing ? 0 : MF_GRAYED), kMenuInstall, (L"Install folder: " + g_cfg.data.wstring() + L"...").c_str());
  AppendMenuW(m, MF_STRING | (!busy && !playing && g_ready && inputs_ok() ? 0 : MF_GRAYED), kMenuRebuild, L"Rebuild the game");
  AppendMenuW(m, MF_STRING | (data ? 0 : MF_GRAYED), kMenuOpenLog, L"Open the setup log");
  AppendMenuW(m, MF_STRING, kMenuWhichFiles, L"Which files does the port need?");
  RECT r{};
  GetWindowRect(item(kMore), &r);
  TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, r.left, r.top, 0, g_wnd, nullptr);
  DestroyMenu(m);  // its submenus too
}

void start_setup() {
  g_note.clear();
  g_setup.start(g_cfg, [] { PostMessageW(g_wnd, WM_SETUP, 0, 0); });
  update_buttons();
}

void on_setup_update() {
  SetupView v = g_setup.view();
  SendMessageW(item(kProgress), PBM_SETPOS, WPARAM(int(v.fraction * 1000)), 0);
  size_t done = 0;
  for (const auto& s : v.steps) done += s.state == StepState::Done || s.state == StepState::Skipped;
  if (v.running) set_text(kStatus, widen(ac4::strprintf("Step %zu of %zu: ", std::min(done + 1, v.steps.size()), v.steps.size()) + v.status));
  if (v.finished) {
    if (!v.ok) g_note = v.cancelled ? L"Cancelled. Set up continues where it stopped." : widen(v.error) + L" (More > Open the setup log)";
    update_buttons();
    if (v.ok) FlashWindow(g_wnd, TRUE);
  }
}

// The check results: one line per folder (update_buttons adds how to get missing files).
void show_checks() {
  Status d, m;
  {
    std::lock_guard lk(g_check_mu);
    d = g_disc_check;
    m = g_files_check;
  }
  set_text(kDiscState, widen(d.text));
  set_text(kFilesState, widen(m.text));
  for (int id : {kDiscState, kFilesState}) InvalidateRect(item(id), nullptr, TRUE);
}

Verdict verdict_of(int id) {
  std::lock_guard lk(g_check_mu);
  if (!g_checked) return Verdict::Empty;
  return id == kDiscState ? g_disc_check.verdict : g_files_check.verdict;
}
COLORREF verdict_color(Verdict v) {
  return v == Verdict::Ok ? RGB(0, 120, 40) : v == Verdict::Bad ? RGB(190, 30, 30) : GetSysColor(COLOR_GRAYTEXT);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);
      if (code == EN_KILLFOCUS && (id == kDiscEdit || id == kFilesEdit)) {
        if (get_text(id) != (id == kDiscEdit ? g_cfg.disc : g_cfg.files).wstring()) refresh_paths_from_ui();
        return 0;
      }
      if (code == CBN_SELCHANGE && (id == kFps || id == kMode || id == kResolution)) {
        save_setting(id);
        update_buttons();
        return 0;
      }
      if (code != BN_CLICKED && lp != 0) break;  // menu items arrive with lp 0
      if (id >= kMenuScale && id < kMenuScale + int(g_scale_items.size())) {
        int pct = g_scale_items[size_t(id - kMenuScale)];
        edit_settings([&](ac4::Ini& ini) { ini.set("display", "render_scale", std::to_string(pct)); });
        return 0;
      }
      if (id >= kMenuMonitor && id < kMenuMonitor + int(g_monitor_items.size())) {
        const MonitorItem& mi = g_monitor_items[size_t(id - kMenuMonitor)];
        ac4::settings::Display d = read_display();
        d.monitor = mi.primary ? "" : narrow(mi.device);
        d.fs_width = d.fs_height = d.fs_hz = 0;  // that monitor's desktop mode
        edit_settings([&](ac4::Ini& ini) {
          ini.set("display", "monitor", d.monitor);
          ini.set("display", "fullscreen_mode", ac4::settings::fullscreen_text(d));
        });
        fill_resolutions(d);
        return 0;
      }
      if (id >= kMenuPreset && id < kMenuPreset + int(std::size(kPresets))) {
        const Preset& p = kPresets[id - kMenuPreset];
        if (edit_settings([&](ac4::Ini& ini) {
              ini.set("display", "fps", p.fps);
              ini.set("display", "render_scale", std::to_string(p.render_scale));
            }))
          load_settings_controls();
        return 0;
      }
      switch (id) {
        case kDiscBrowse: {
          fs::path p = pick_folder(L"The game's folder (it contains PS3_GAME)", g_cfg.disc);
          if (!p.empty()) set_text(kDiscEdit, p.wstring()), refresh_paths_from_ui();
          break;
        }
        case kFilesBrowse: {
          fs::path p = pick_folder(L"The folder with the decrypted files", g_cfg.files.empty() ? g_cfg.disc : g_cfg.files);
          if (!p.empty()) set_text(kFilesEdit, p.wstring()), refresh_paths_from_ui();
          break;
        }
        case kMenuWhichFiles:
          MessageBoxW(g_wnd, files_help().c_str(), L"The files the port needs", MB_ICONINFORMATION);
          break;
        case kPrepFiles:
          if (IsWindowEnabled(item(kPrepFiles)) && !g_prep.running() && !g_setup.running()) prepare_files();
          break;
        case kPrepare:
          if (IsWindowEnabled(item(kPrepare))) start_setup();
          break;
        case kCancel:
          if (g_prep.running()) g_prep.cancel();
          else g_setup.cancel();
          break;
        case kMore:
          more_menu();
          break;
        case kPlay:
          if (IsWindowEnabled(item(kPlay))) play(false);
          break;
        case kMenuSafePlay:
          play(true);
          break;
        case kMenuRebuild:
          start_setup();
          break;
        case kMenuOpenLog: {
          fs::path log = g_cfg.data / "launcher.log";
          if (fs::exists(log)) ShellExecuteW(g_wnd, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
        }
        case kMenuInstall: {
          fs::path p = pick_folder(L"Install folder (tools, prepared files, build)", g_cfg.data);
          if (p.empty()) break;
          if (!writable(p)) {
            MessageBoxW(g_wnd, L"This folder cannot be written. Choose another one.", kTitle, MB_ICONWARNING);
            break;
          }
          g_cfg.data = p;
          save_paths();
          move_to_user_dir();
          load_settings_controls();
          update_buttons();
          break;
        }
        case kMenuResetSettings: {
          if (MessageBoxW(g_wnd,
                          L"Reset every setting (display, frame rate, mouse, key bindings) to its default?\n"
                          L"The current file is kept as settings.ini.bak.",
                          kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
          std::error_code ec;
          fs::path bak = settings_path();
          bak += ".bak";
          fs::rename(settings_path(), bak, ec);
          load_settings_controls();  // creates the file with the defaults
          update_buttons();
          break;
        }
        case kMenuSavesOpen: {
          std::error_code ec;
          fs::create_directories(saves_dir(), ec);
          ShellExecuteW(g_wnd, L"explore", saves_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          break;
        }
        case kMenuSavesBackup: {
          fs::path to = backups_dir() / (L"saves-" + stamp_now());
          size_t n = copy_saves(saves_dir(), to);
          std::wstring msg = n ? L"Saves backed up to\n" + to.wstring() : L"There are no saves yet.";
          MessageBoxW(g_wnd, msg.c_str(), kTitle, MB_ICONINFORMATION);
          break;
        }
        case kMenuSavesRestore: {
          fs::path from = pick_folder(L"Backup to restore (a saves-... folder)", backups_dir());
          if (from.empty()) break;
          std::error_code ec;
          bool any = false;
          for (const auto& e : fs::directory_iterator(from, ec))
            any |= e.is_directory() && e.path().filename().wstring().rfind(kSavePrefix, 0) == 0;
          if (!any) {
            MessageBoxW(g_wnd, L"No saves of this game in that folder.", kTitle, MB_ICONWARNING);
            break;
          }
          if (MessageBoxW(g_wnd, L"Replace the current saves with this backup?\nThe current saves are backed up first.", kTitle,
                          MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
          copy_saves(saves_dir(), backups_dir() / (L"saves-before-restore-" + stamp_now()));
          for (const auto& e : fs::directory_iterator(saves_dir(), ec))
            if (e.is_directory() && e.path().filename().wstring().rfind(kSavePrefix, 0) == 0) fs::remove_all(e.path(), ec);
          size_t n = copy_saves(from, saves_dir());
          MessageBoxW(g_wnd, (std::to_wstring(n) + L" save(s) restored.").c_str(), kTitle, MB_ICONINFORMATION);
          break;
        }
      }
      return 0;
    }
    case WM_SETUP:
      on_setup_update();
      return 0;
    case WM_PREP:
      on_prep_update();
      return 0;
    case WM_CHECKED:
      if (int(wp) != g_check_gen) return 0;
      show_checks();
      update_buttons();
      return 0;
    case WM_GAME_EXIT: {
      if (g_game) CloseHandle(g_game);
      g_game = nullptr;
      ShowWindow(g_wnd, SW_RESTORE);
      load_settings_controls();  // PC SETTINGS may have changed them
      if (wp != 0) {
        // The error as the game logged it (logs carry no personal data: they can be shared).
        fs::path log = newest_run_log();
        std::ifstream in(log);
        std::string line, error;
        while (std::getline(in, line))
          if (line.find(" F ") != std::string::npos && line.find("] ") != std::string::npos) {
            error = line.substr(line.find("] ") + 2);
            break;
          }
        g_note = L"The game stopped with an error" + (error.empty() ? std::wstring(L".") : L": " + widen(error)) + L" Log: " + log.wstring();
      }
      update_buttons();
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      int id = GetDlgCtrlID(HWND(lp));
      HDC dc = HDC(wp);
      SetBkMode(dc, TRANSPARENT);
      if (id == kDiscState || id == kFilesState) SetTextColor(dc, verdict_color(verdict_of(id)));
      return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DPICHANGED: {
      g_dpi = HIWORD(wp);
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      make_fonts();
      layout();
      return 0;
    }
    case WM_CLOSE:
      if ((g_setup.running() || g_prep.running()) &&
          MessageBoxW(h, L"The launcher is still working. Stop it and close?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
      g_setup.cancel();
      g_prep.cancel();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show_cmd) {
  g_inst = inst;
  // One launcher per install (a copy of the port in another folder may run beside it).
  wchar_t self_path[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, self_path, DWORD(std::size(self_path)));
  std::wstring self_key = self_path;
  for (auto& ch : self_key) ch = wchar_t(towlower(ch));
  unsigned long long fnv = 1469598103934665603ull;
  for (wchar_t ch : self_key) fnv = (fnv ^ unsigned(ch)) * 1099511628211ull;
  HANDLE single = CreateMutexW(nullptr, TRUE, (L"Local\\AC4-Port-Launcher-" + std::to_wstring(fnv)).c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    struct Find {
      std::wstring self;
      HWND found = nullptr;
    } find{self_key};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
      auto* f = reinterpret_cast<Find*>(lp);
      wchar_t cls[64] = {};
      GetClassNameW(h, cls, 64);
      DWORD pid = 0;
      GetWindowThreadProcessId(h, &pid);
      HANDLE p = std::wcscmp(cls, L"Ac4Launcher") == 0 ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
      if (!p) return TRUE;
      wchar_t path[MAX_PATH * 4];
      DWORD n = DWORD(std::size(path));
      std::wstring other = QueryFullProcessImageNameW(p, 0, path, &n) ? path : L"";
      CloseHandle(p);
      for (auto& ch : other) ch = wchar_t(towlower(ch));
      if (other == f->self) f->found = h;
      return f->found ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&find));
    if (find.found) SetForegroundWindow(find.found);
    return 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  g_src = find_sources();
  if (g_src.empty()) {
    MessageBoxW(nullptr, L"Put AC4-Launcher.exe in the port's folder (the one with CMakeLists.txt, data and src).", kTitle, MB_ICONERROR);
    return 1;
  }
  // The launcher's own record of the chosen locations: beside it when that folder is writable.
  wchar_t self[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  fs::path exe_dir = fs::path(self).parent_path();
  g_ini_path = (writable(exe_dir) ? exe_dir : local_appdata()) / "launcher.ini";
  load_paths();
  g_cfg.src = g_src;
  if (g_cfg.data.empty()) g_cfg.data = writable(g_src / "local") ? g_src / "local" : local_appdata();
  {
    std::error_code ec;
    if (g_cfg.files.empty() && fs::is_regular_file(g_cfg.data / "decrypted" / "EBOOT.elf", ec)) g_cfg.files = g_cfg.data / "decrypted";
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"Ac4Launcher";
  RegisterClassExW(&wc);
  g_dpi = GetDpiForSystem();
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, S(kClientW), S(kClientH)};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
  g_wnd = CreateWindowExW(0, wc.lpszClassName, L"ARMORED CORE 4 - PC port " AC4_VERSION_W, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  UINT dpi = GetDpiForWindow(g_wnd);
  if (dpi != g_dpi) {  // the window opened on a monitor with another scale
    g_dpi = dpi;
    r = RECT{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
  }
  create_controls();
  make_fonts();
  layout();
  set_text(kDiscEdit, g_cfg.disc.wstring());
  set_text(kFilesEdit, g_cfg.files.wstring());
  refresh_paths_from_ui();
  load_settings_controls();
  update_buttons();
  ShowWindow(g_wnd, show_cmd);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(g_wnd, &m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  CoUninitialize();
  CloseHandle(single);
  return 0;
}
