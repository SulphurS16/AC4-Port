// The player's install as Windows sees it: the per-user record that lets a newer launcher, extracted
// into another folder, find the install of an earlier one; where the game data should not go
// (Downloads, a cloud-synced folder, Temp); moving the game data within a drive; the shortcuts the
// launcher made; removing everything the launcher and the game put on the disk.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

// This Windows user's folders. Development builds: the environment variable AC4_LAUNCHER_HOME=DIR
// stands in for the user's profile in tests (DIR\AppData\Local, DIR\Desktop, DIR\Start Menu\Programs,
// DIR\Downloads, DIR\Saved Games; DIR\OneDrive is then the only cloud-synced folder), so a test never
// touches the real desktop, Start menu, saves or install record.
enum class UserFolder { LocalAppData, Desktop, StartMenuPrograms, Downloads, SavedGames };
fs::path known_folder(UserFolder f);
fs::path port_appdata();  // <Local AppData>\AC4 Port: the launcher's per-user files
fs::path test_home();     // AC4_LAUNCHER_HOME (development builds), else empty

// child lies in parent (or is it): absolute, case-insensitive.
bool inside(const fs::path& child, const fs::path& parent);
bool same_volume(const fs::path& a, const fs::path& b);

// Places the game data should not go: Windows' cleanup can delete old files in Downloads (Storage
// Sense), a cloud-synced folder (OneDrive) would upload gigabytes and can make files online-only,
// Temp is emptied.
enum class Risk { None, Downloads, Cloud, Temp };
Risk place_risk(const fs::path& dir);
std::wstring risk_text(Risk r);  // why, in a sentence for the player

// The game data folder holds an install: files prepared (by Prepare files or imported) or built.
bool has_install(const fs::path& data);
bool has_build(const fs::path& data);  // Set up finished there at least once

// An install of this Windows user, as a launcher left it.
struct Install {
  fs::path launcher;     // the launcher that used it last (may be gone)
  fs::path data;         // the game data folder
  fs::path disc, files, rpcs3, pup;
  std::string version;   // of that launcher ("" unknown: launchers before the record)
  long long when = 0;    // last change of the install (file time ticks)
};
// The earlier installs, newest first: the record, then the launcher.ini files of launchers from
// before the record (in the folders beside this launcher's folder, in Downloads and under Local
// AppData). Only folders that still hold an install; self_exe's own folder is skipped.
std::vector<Install> earlier_installs(const fs::path& self_exe);
void record_install(const Install& i);
void forget_install(const fs::path& data);  // when the record names this data folder

// The shortcuts the launcher made (a desktop shortcut and a Start menu entry), per Windows user.
struct Shortcuts {
  std::string answer;     // "" not asked yet, "yes", "no"
  fs::path desktop, start_menu, target;
  std::string version;    // the launcher they open
  bool made() const { return answer == "yes"; }
};
Shortcuts read_shortcuts();
void record_shortcut_answer(const std::string& answer);  // "no": asked, declined
// Makes (or remakes) both, opening exe; records them. Returns "" or what went wrong.
std::string make_shortcuts(const fs::path& exe, const std::string& version);
// Deletes the ones it made (left alone when the player pointed them elsewhere); the answer becomes "no".
void remove_shortcuts();
// The shortcuts it made open exe from now on when this launcher is newer than the one they open,
// or that one is gone. Returns true when they changed.
bool retarget_shortcuts(const fs::path& exe, const std::string& version);
int compare_versions(const std::string& a, const std::string& b);  // <0, 0, >0 ("1.10.0" > "1.9.2")

// What removing the game data deletes, by kind, with sizes.
struct RemoveItem {
  std::wstring what;            // for the player
  std::vector<fs::path> paths;  // files and folders
  unsigned long long bytes = 0;
};
unsigned long long path_bytes(const fs::path& p);
// The launcher's and the game's files in the data folder (the player's own decrypted files outside
// it are never included) and the shortcuts the launcher made.
std::vector<RemoveItem> removal_items(const fs::path& data);
// Deletes them, then the data folder itself when it is left empty. Paths outside the data folder
// other than the recorded shortcuts are refused. Returns what could not be deleted.
std::vector<fs::path> remove_items(const std::vector<RemoveItem>& items, const fs::path& data);
// The player's saves and settings folder, moved aside as a backup ("... backup YYYYMMDD-HHMMSS"
// beside it); saves the game kept in the game data folder (hdd0\home) go into it too. Returns the
// backup folder, or "" with *error set.
fs::path back_up_user_dir(const fs::path& user_dir, const fs::path& data, std::wstring* error);

// Moves the game data folder to another folder on the same drive (a rename, at once). The build
// folder's CMake cache stays behind (it holds the old path), so the next Set up builds the game
// again in full. Returns "" or what went wrong.
std::string move_data(const fs::path& from, const fs::path& to);

}  // namespace launcher
