#include "launcher/install.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <ctime>

#include "common/ini.h"
#include "launcher/process.h"

namespace launcher {
namespace {

constexpr const wchar_t* kShortcutName = L"ARMORED CORE 4.lnk";
constexpr const wchar_t* kLauncherName = L"AC4-Launcher.exe";

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

std::wstring lower(std::wstring s) {
  for (auto& ch : s) ch = wchar_t(towlower(ch));
  return s;
}

bool same_path(const fs::path& a, const fs::path& b) { return !a.empty() && !b.empty() && inside(a, b) && inside(b, a); }

fs::path test_home_dir() {
  return {};
}

std::wstring env(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
  return n && n < std::size(buf) ? std::wstring(buf) : std::wstring();
}

std::wstring user_sid() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
  std::wstring out;
  DWORD n = 0;
  GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
  std::vector<unsigned char> buf(n ? n : 1);
  if (n && GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
    LPWSTR s = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
      out = s;
      LocalFree(s);
    }
  }
  CloseHandle(tok);
  return out;
}

// The folders a cloud service syncs for this user: OneDrive's (its environment variables and
// accounts), and every sync root registered with Windows' cloud files API (OneDrive and others).
std::vector<fs::path> cloud_roots() {
  if (fs::path h = test_home_dir(); !h.empty()) return {h / L"OneDrive"};
  std::vector<fs::path> out;
  for (const wchar_t* v : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"})
    if (std::wstring p = env(v); !p.empty()) out.push_back(p);
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0, KEY_READ, &k) == ERROR_SUCCESS) {
    for (DWORD i = 0;; ++i) {
      wchar_t name[256];
      DWORD nn = DWORD(std::size(name));
      if (RegEnumKeyExW(k, i, name, &nn, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      wchar_t val[MAX_PATH * 2];
      DWORD bytes = sizeof val;
      if (RegGetValueW(k, name, L"UserFolder", RRF_RT_REG_SZ, nullptr, val, &bytes) == ERROR_SUCCESS && val[0]) out.push_back(val);
    }
    RegCloseKey(k);
  }
  std::wstring sid = user_sid();
  if (!sid.empty() &&
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SyncRootManager", 0, KEY_READ, &k) ==
          ERROR_SUCCESS) {
    for (DWORD i = 0;; ++i) {
      wchar_t name[512];
      DWORD nn = DWORD(std::size(name));
      if (RegEnumKeyExW(k, i, name, &nn, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      std::wstring sub = std::wstring(name) + L"\\UserSyncRoots";
      wchar_t val[MAX_PATH * 2];
      DWORD bytes = sizeof val;
      if (RegGetValueW(k, sub.c_str(), sid.c_str(), RRF_RT_REG_SZ, nullptr, val, &bytes) == ERROR_SUCCESS && val[0]) out.push_back(val);
    }
    RegCloseKey(k);
  }
  return out;
}

std::wstring volume_of(fs::path p) {
  std::error_code ec;
  p = fs::absolute(p, ec);
  while (!p.empty() && !fs::exists(p, ec) && p != p.parent_path()) p = p.parent_path();
  wchar_t vol[MAX_PATH + 1];
  if (p.empty() || !GetVolumePathNameW(p.c_str(), vol, MAX_PATH)) return L"";
  wchar_t guid[64];
  if (GetVolumeNameForVolumeMountPointW(vol, guid, DWORD(std::size(guid)))) return lower(guid);
  return lower(vol);
}

long long mtime(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  return ec ? 0 : (long long)t.time_since_epoch().count();
}

// The record of this Windows user's install and the shortcuts (Local AppData; never in a folder
// the player may delete with a release).
fs::path record_path() { return port_appdata() / L"installs.ini"; }

ac4::Ini load_record() {
  try {
    ac4::Ini r = ac4::Ini::load(record_path());
    if (r.empty())
      r = ac4::Ini::parse(
          "# ARMORED CORE 4 launchers of this Windows user: the install they set up (a newer\n"
          "# launcher, extracted into another folder, offers to use it) and the shortcuts they made.\n");
    return r;
  } catch (const std::exception&) {
    return ac4::Ini::parse("");
  }
}

void save_record(const ac4::Ini& r) {
  try {
    std::error_code ec;
    fs::create_directories(port_appdata(), ec);
    r.save(record_path());
  } catch (const std::exception&) {
  }
}

fs::path path_of(const ac4::Ini& ini, const char* section, const char* key) { return fs::path(widen(ini.get_or(section, key, ""))); }

std::string create_link(const fs::path& lnk, const fs::path& exe) {
  std::error_code ec;
  fs::create_directories(lnk.parent_path(), ec);
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return "Windows could not make a shortcut.";
  sl->SetPath(exe.c_str());
  sl->SetWorkingDirectory(exe.parent_path().c_str());
  sl->SetDescription(L"ARMORED CORE 4 PC port: the launcher (PLAY starts the game)");
  sl->SetIconLocation(exe.c_str(), 0);
  IPersistFile* pf = nullptr;
  HRESULT hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
  if (SUCCEEDED(hr)) {
    hr = pf->Save(lnk.c_str(), TRUE);
    pf->Release();
  }
  sl->Release();
  return SUCCEEDED(hr) ? "" : "The shortcut " + narrow(lnk.wstring()) + " could not be saved.";
}

// The program a shortcut opens ("" when it cannot be read).
fs::path link_target(const fs::path& lnk) {
  IShellLinkW* sl = nullptr;
  fs::path out;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return out;
  IPersistFile* pf = nullptr;
  if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
    if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
      wchar_t p[MAX_PATH * 2] = {};
      if (SUCCEEDED(sl->GetPath(p, int(std::size(p)), nullptr, SLGP_RAWPATH))) out = p;
    }
    pf->Release();
  }
  sl->Release();
  return out;
}

// A shortcut the launcher made and the player left alone: it still opens a launcher of the port.
bool ours(const fs::path& lnk) { return is_file(lnk) && lower(link_target(lnk).filename().wstring()) == lower(kLauncherName); }

}  // namespace

fs::path test_home() { return test_home_dir(); }

fs::path known_folder(UserFolder f) {
  if (fs::path h = test_home_dir(); !h.empty()) {
    switch (f) {
      case UserFolder::LocalAppData: return h / L"AppData" / L"Local";
      case UserFolder::Desktop: return h / L"Desktop";
      case UserFolder::StartMenuPrograms: return h / L"Start Menu" / L"Programs";
      case UserFolder::Downloads: return h / L"Downloads";
      case UserFolder::SavedGames: return h / L"Saved Games";
    }
  }
  const KNOWNFOLDERID* id = &FOLDERID_LocalAppData;
  switch (f) {
    case UserFolder::LocalAppData: id = &FOLDERID_LocalAppData; break;
    case UserFolder::Desktop: id = &FOLDERID_Desktop; break;
    case UserFolder::StartMenuPrograms: id = &FOLDERID_Programs; break;
    case UserFolder::Downloads: id = &FOLDERID_Downloads; break;
    case UserFolder::SavedGames: id = &FOLDERID_SavedGames; break;
  }
  PWSTR p = nullptr;
  fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(*id, f == UserFolder::SavedGames ? KF_FLAG_CREATE : 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

fs::path port_appdata() { return known_folder(UserFolder::LocalAppData) / L"AC4 Port"; }

bool inside(const fs::path& child, const fs::path& parent) {
  if (child.empty() || parent.empty()) return false;
  std::error_code ec;
  std::wstring c = fs::weakly_canonical(fs::absolute(child, ec), ec).wstring(), p = fs::weakly_canonical(fs::absolute(parent, ec), ec).wstring();
  for (auto* s : {&c, &p}) {
    *s = lower(*s);
    while (s->size() > 3 && (s->back() == L'\\' || s->back() == L'/')) s->pop_back();
  }
  return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && (c[p.size()] == L'\\' || c[p.size()] == L'/' || p.back() == L'\\'));
}

bool same_volume(const fs::path& a, const fs::path& b) {
  std::wstring x = volume_of(a), y = volume_of(b);
  return !x.empty() && x == y;
}

Risk place_risk(const fs::path& dir) {
  wchar_t temp[MAX_PATH + 1];
  if (GetTempPathW(DWORD(std::size(temp)), temp) && inside(dir, temp)) return Risk::Temp;
  for (const fs::path& root : cloud_roots())
    if (inside(dir, root)) return Risk::Cloud;
  if (fs::path dl = known_folder(UserFolder::Downloads); !dl.empty() && inside(dir, dl)) return Risk::Downloads;
  return Risk::None;
}

std::wstring risk_text(Risk r) {
  switch (r) {
    case Risk::Downloads:
      return L"It is in your Downloads folder: Windows' cleanup (Storage Sense) and cleanup tools can delete old files there.";
    case Risk::Cloud:
      return L"OneDrive (or another cloud service) syncs this folder: the game data would be uploaded, and its files can "
             L"become online-only.";
    case Risk::Temp: return L"It is in a temporary folder, which Windows empties.";
    case Risk::None: break;
  }
  return L"";
}

bool has_build(const fs::path& data) { return is_file(data / L"state" / L"built.txt"); }

bool has_install(const fs::path& data) {
  return has_build(data) || is_file(data / L"game" / L"BLUS30027" / L"eboot.elf") || is_file(data / L"decrypted" / L"EBOOT.elf");
}

std::vector<Install> earlier_installs(const fs::path& self_exe) {
  std::vector<Install> out;
  fs::path self_dir = self_exe.parent_path();
  auto add = [&](Install i) {
    if (i.data.empty() || !has_install(i.data) || inside(i.data, self_dir)) return;  // this launcher's own folder: not an earlier install
    for (const Install& o : out)
      if (same_path(o.data, i.data)) return;
    for (const fs::path& p : {i.data / L"state" / L"built.txt", i.data / L"launcher.log", i.data / L"game" / L"BLUS30027" / L"manifest.txt",
                              i.data / L"decrypted" / L"EBOOT.elf"})
      i.when = std::max(i.when, mtime(p));
    out.push_back(i);
  };
  ac4::Ini rec = load_record();
  if (rec.has_section("install")) {
    Install i;
    i.data = path_of(rec, "install", "data");
    i.launcher = path_of(rec, "install", "launcher");
    i.disc = path_of(rec, "install", "disc");
    i.files = path_of(rec, "install", "files");
    i.rpcs3 = path_of(rec, "install", "rpcs3");
    i.pup = path_of(rec, "install", "pup");
    i.version = rec.get_or("install", "version", "");
    add(i);
  }
  // Launchers from before the record left their choices only in launcher.ini beside themselves.
  std::vector<fs::path> dirs;
  auto children = [&](const fs::path& parent) {
    std::error_code ec;
    if (parent.empty()) return;
    for (const auto& e : fs::directory_iterator(parent, fs::directory_options::skip_permission_denied, ec))
      if (e.is_directory(ec)) dirs.push_back(e.path());
  };
  dirs.push_back(self_dir.parent_path());  // extracted into a folder inside the earlier one
  children(self_dir.parent_path());
  children(known_folder(UserFolder::Downloads));
  dirs.push_back(port_appdata());
  for (const fs::path& d : dirs) {
    if (same_path(d, self_dir) || !is_file(d / L"launcher.ini")) continue;
    try {
      ac4::Ini li = ac4::Ini::load(d / L"launcher.ini");
      Install i;
      i.launcher = d / kLauncherName;
      i.data = path_of(li, "paths", "install");
      i.disc = path_of(li, "paths", "disc");
      i.files = path_of(li, "paths", "files");
      i.rpcs3 = path_of(li, "paths", "rpcs3");
      i.pup = path_of(li, "paths", "pup");
      add(i);
    } catch (const std::exception&) {
    }
  }
  std::sort(out.begin(), out.end(), [](const Install& a, const Install& b) { return a.when > b.when; });
  return out;
}

void record_install(const Install& i) {
  ac4::Ini r = load_record();
  r.set("install", "data", narrow(i.data.wstring()));
  r.set("install", "launcher", narrow(i.launcher.wstring()));
  r.set("install", "version", i.version);
  r.set("install", "disc", narrow(i.disc.wstring()));
  r.set("install", "files", narrow(i.files.wstring()));
  r.set("install", "rpcs3", narrow(i.rpcs3.wstring()));
  r.set("install", "pup", narrow(i.pup.wstring()));
  save_record(r);
}

void forget_install(const fs::path& data) {
  ac4::Ini r = load_record();
  if (!same_path(path_of(r, "install", "data"), data)) return;
  for (const char* k : {"data", "launcher", "version", "disc", "files", "rpcs3", "pup"}) r.remove("install", k);
  save_record(r);
}

Shortcuts read_shortcuts() {
  ac4::Ini r = load_record();
  Shortcuts s;
  s.answer = r.get_or("shortcuts", "answer", "");
  s.desktop = path_of(r, "shortcuts", "desktop");
  s.start_menu = path_of(r, "shortcuts", "start_menu");
  s.target = path_of(r, "shortcuts", "target");
  s.version = r.get_or("shortcuts", "version", "");
  return s;
}

void record_shortcut_answer(const std::string& answer) {
  ac4::Ini r = load_record();
  r.set("shortcuts", "answer", answer);
  save_record(r);
}

std::string make_shortcuts(const fs::path& exe, const std::string& version) {
  fs::path desktop = known_folder(UserFolder::Desktop) / kShortcutName, start = known_folder(UserFolder::StartMenuPrograms) / kShortcutName;
  std::string err = create_link(desktop, exe);
  std::string err2 = create_link(start, exe);
  ac4::Ini r = load_record();
  r.set("shortcuts", "answer", "yes");
  r.set("shortcuts", "desktop", err.empty() ? narrow(desktop.wstring()) : "");
  r.set("shortcuts", "start_menu", err2.empty() ? narrow(start.wstring()) : "");
  r.set("shortcuts", "target", narrow(exe.wstring()));
  r.set("shortcuts", "version", version);
  save_record(r);
  return !err.empty() ? err : err2;
}

void remove_shortcuts() {
  Shortcuts s = read_shortcuts();
  for (const fs::path& lnk : {s.desktop, s.start_menu}) {
    std::error_code ec;
    if (ours(lnk)) fs::remove(lnk, ec);
  }
  ac4::Ini r = load_record();
  r.set("shortcuts", "answer", "no");
  for (const char* k : {"desktop", "start_menu", "target", "version"}) r.remove("shortcuts", k);
  save_record(r);
}

int compare_versions(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && isdigit(static_cast<unsigned char>(a[i]))) x = x * 10 + unsigned(a[i++] - '0');
    while (j < b.size() && isdigit(static_cast<unsigned char>(b[j]))) y = y * 10 + unsigned(b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    while (i < a.size() && !isdigit(static_cast<unsigned char>(a[i]))) ++i;
    while (j < b.size() && !isdigit(static_cast<unsigned char>(b[j]))) ++j;
  }
  return 0;
}

bool retarget_shortcuts(const fs::path& exe, const std::string& version) {
  Shortcuts s = read_shortcuts();
  if (!s.made() || same_path(s.target, exe)) return false;
  if (compare_versions(version, s.version) <= 0 && is_file(s.target)) return false;  // an older or equal launcher: theirs stay
  bool changed = false;
  for (const fs::path& lnk : {s.desktop, s.start_menu})
    if (ours(lnk) && create_link(lnk, exe).empty()) changed = true;  // one the player deleted stays deleted
  ac4::Ini r = load_record();
  r.set("shortcuts", "target", narrow(exe.wstring()));
  r.set("shortcuts", "version", version);
  save_record(r);
  return changed;
}

unsigned long long path_bytes(const fs::path& p) {
  std::error_code ec;
  if (fs::is_regular_file(p, ec)) return fs::file_size(p, ec);
  unsigned long long n = 0;
  for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) n += it->file_size(e2);
  }
  return n;
}

namespace {
// A file or folder under one of the launcher's own names that the launcher or the game made (an
// install folder chosen with earlier versions may be a folder that holds other things too).
bool looks_ours(const fs::path& p) {
  std::error_code ec;
  std::wstring n = lower(p.filename().wstring());
  auto has = [&](const wchar_t* child) { return fs::exists(p / child, ec); };
  auto any_child = [&](const wchar_t* prefix) {
    for (const auto& e : fs::directory_iterator(p, ec))
      if (lower(e.path().filename().wstring()).rfind(prefix, 0) == 0) return true;
    return false;
  };
  if (n == L"tools") return any_child(L"llvm-mingw") || any_child(L"cmake-") || has(L"ninja");
  if (n == L"downloads") return fs::is_empty(p, ec) || any_child(L"llvm-mingw") || any_child(L"cmake-") || any_child(L"ninja");
  if (n == L"game") return has(L"BLUS30027") || has(L"dev_flash");
  if (n == L"decrypted") return has(L"EBOOT.elf");
  if (n == L"build") return has(L"CMakeCache.txt") || has(L"build.ninja") || has(L"gen") || has(L"ac4.exe");
  if (n == L"state") return has(L"built.txt") || has(L"import.txt") || has(L"recompile.txt") || has(L"spu.txt");
  if (n == L"source") return has(L".launcher-source");
  if (n == L"runs") return fs::is_empty(p, ec) || any_child(L"20");  // run folders: <UTC date>-<time>
  if (n == L"config") return has(L"settings.ini") || has(L"disc_root.txt");  // the game's own, without --user (the shader step)
  if (n == L"generated") return has(L"systemsetting.000") || has(L"systemsetting.000.txt");  // PC SETTINGS' menu layout (from the disc)
  return true;  // names only the port uses (rpcs3-work, spu_cache, shader_cache, hdd1, ..., launcher.log)
}
}  // namespace

std::vector<RemoveItem> removal_items(const fs::path& data) {
  struct Group {
    const wchar_t* what;
    std::vector<const wchar_t*> names;
  };
  const Group groups[] = {
      {L"The build tools", {L"tools", L"downloads"}},
      {L"The prepared game files", {L"game", L"decrypted", L"rpcs3-work"}},
      {L"The built game and its caches",
       {L"build", L"state", L"generated", L"spu_cache", L"shader_cache", L"shader_records", L"hdd1", L"hdd0\\game", L"config"}},
      {L"The port's unpacked sources", {L"source", L"source.new", L"source.zip.tmp"}},
      {L"The logs", {L"runs", L"launcher.log", L"prepare-files.log"}},
  };
  std::vector<RemoveItem> out;
  std::error_code ec;
  for (const Group& g : groups) {
    RemoveItem it;
    it.what = g.what;
    for (const wchar_t* n : g.names)
      if (fs::exists(data / n, ec) && looks_ours(data / n)) {
        it.paths.push_back(data / n);
        it.bytes += path_bytes(data / n);
      }
    if (!it.paths.empty()) out.push_back(it);
  }
  Shortcuts s = read_shortcuts();
  RemoveItem lnk;
  lnk.what = L"The shortcuts the launcher made";
  for (const fs::path& p : {s.desktop, s.start_menu})
    if (s.made() && ours(p)) lnk.paths.push_back(p);
  if (!lnk.paths.empty()) out.push_back(lnk);
  return out;
}

std::vector<fs::path> remove_items(const std::vector<RemoveItem>& items, const fs::path& data) {
  std::vector<fs::path> failed;
  std::error_code e0;
  if (fs::exists(data.parent_path() / L".git", e0)) {  // a development checkout's local folder: never
    for (const RemoveItem& it : items) failed.insert(failed.end(), it.paths.begin(), it.paths.end());
    return failed;
  }
  Shortcuts s = read_shortcuts();
  bool links = false;
  for (const RemoveItem& it : items)
    for (const fs::path& p : it.paths) {
      if (s.made() && (same_path(p, s.desktop) || same_path(p, s.start_menu))) {
        links = true;
        continue;
      }
      std::error_code ec;
      if (!inside(p, data) || same_path(p, data)) {  // only what lies in the data folder
        failed.push_back(p);
        continue;
      }
      fs::remove_all(p, ec);
      if (fs::exists(p, ec)) failed.push_back(p);
    }
  if (links) remove_shortcuts();
  std::error_code ec;
  for (const fs::path& d : {data / L"hdd0", data})
    if (fs::is_directory(d, ec) && fs::is_empty(d, ec)) fs::remove(d, ec);
  return failed;
}

fs::path back_up_user_dir(const fs::path& user_dir, const fs::path& data, std::wstring* error) {
  std::error_code ec;
  fs::path old_saves = data / L"hdd0" / L"home";
  bool user = fs::exists(user_dir, ec), old = !data.empty() && fs::exists(old_saves, ec);
  if (!user && !old) return {};
  std::time_t t = std::time(nullptr);
  wchar_t stamp[32];
  std::wcsftime(stamp, std::size(stamp), L"%Y%m%d-%H%M%S", std::localtime(&t));
  fs::path backup = user_dir.parent_path() / (user_dir.filename().wstring() + L" backup " + stamp);
  if (user) {
    fs::rename(user_dir, backup, ec);
    if (ec) {  // a file in use: copy, then delete
      ec.clear();
      fs::copy(user_dir, backup, fs::copy_options::recursive, ec);
      if (ec) {
        *error = L"Your saves and settings could not be backed up (" + widen(ec.message()) + L"); nothing was removed.";
        fs::remove_all(backup, ec);
        return {};
      }
      fs::remove_all(user_dir, ec);
    }
  }
  if (old) {
    fs::create_directories(backup, ec);
    fs::copy(old_saves, backup / L"saves from the game data folder", fs::copy_options::recursive, ec);
    if (!ec) fs::remove_all(old_saves, ec);
  }
  return backup;
}

std::string move_data(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (!fs::is_directory(from, ec)) return narrow(from.wstring()) + " is not there any more.";
  if (inside(to, from) || inside(from, to)) return "The new folder must not lie inside the game data folder, or the other way round.";
  if (!same_volume(from, to)) return "The new folder is on another drive: the game data can only be moved within a drive.";
  if (fs::exists(to, ec)) {
    if (!fs::is_directory(to, ec) || !fs::is_empty(to, ec)) return narrow(to.wstring()) + " is not empty.";
    fs::remove(to, ec);
  }
  fs::create_directories(to.parent_path(), ec);
  if (!MoveFileExW(from.c_str(), to.c_str(), 0))
    return "Moving the game data failed (Windows error " + std::to_string(GetLastError()) +
           "): a file in it may be in use (close the game and programs showing that folder).";
  // CMake's cache holds the old path: the next Set up configures the build again (and builds in full).
  fs::remove(to / L"build" / L"CMakeCache.txt", ec);
  fs::remove_all(to / L"build" / L"CMakeFiles", ec);
  return "";
}

}  // namespace launcher
