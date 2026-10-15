// LV2 kernel services: initialization shared by the syscall modules.
#include <filesystem>

#include "common/file.h"
#include "runtime/config.h"
#include "runtime/game/system_menu.h"
#include "runtime/hle.h"
#include "runtime/log.h"
#include "runtime/vfs.h"

namespace rt {

namespace fs = std::filesystem;

// Installs the prepared update package (game data) into /dev_hdd0/game/<TITLE_ID> the way the
// PS3 system software would, without overwriting files the game has written since.
static void install_update_data() {
  fs::path upd = fs::path(cfg().prepared) / "update";
  std::error_code ec;
  if (!fs::exists(upd, ec)) {
    LOG_W("lv2", "no prepared update under %s; running with disc data only", upd.string().c_str());
    return;
  }
  for (const auto& pkg : fs::directory_iterator(upd, ec)) {
    if (!pkg.is_directory()) continue;
    std::string cid = pkg.path().filename().string();  // UP0001-<TITLE_ID>_00-...
    if (cid.size() < 16) continue;
    std::string title = cid.substr(7, 9);
    fs::path dst = fs::path(cfg().hdd0) / "game" / title;
    size_t copied = 0;
    for (const auto& e : fs::recursive_directory_iterator(pkg.path(), ec)) {
      fs::path rel = fs::relative(e.path(), pkg.path(), ec);
      fs::path to = dst / rel;
      if (e.is_directory()) {
        fs::create_directories(to, ec);
        continue;
      }
      if (fs::exists(to, ec)) continue;
      ac4::guard_write(to);
      fs::create_directories(to.parent_path(), ec);
      fs::copy_file(e.path(), to, ec);
      if (!ec) ++copied;
    }
    LOG_I("lv2", "game data %s from %s: %zu file(s) installed", title.c_str(), cid.c_str(), copied);
  }
}

void lv2_init() {
  vfs::init(cfg().disc_root, cfg().hdd0, cfg().hdd1, cfg().flash, cfg().saves, cfg().test_overlay);
  ac4::add_protected_root(cfg().flash);
  // The SYSTEM menu with its PC SETTINGS row (the port's copy of the menu's archive).
  if (std::string menu = game::system_menu_archive(cfg().disc_root, cfg().data_dir); !menu.empty())
    vfs::replace(game::kSystemMenuArchive, menu);
  install_update_data();
  LOG_I("lv2", "lv2 initialized");
}

}  // namespace rt
