// HLE (cellSysutil): cellGameDataCheckCreate2, cellHddGameCheck, cellSysCacheMount.
// Game data lives under /dev_hdd0/game/<dir> (project-owned local/hdd0); the system cache is
// /dev_hdd1 (local/hdd1), cleared when a different cache id is mounted.
#include <filesystem>

#include "common/file.h"
#include "common/sfo.h"
#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/vfs.h"

namespace rt::game_data {
namespace fs = std::filesystem;
namespace {

constexpr s32 GAMEDATA_ERROR_CBRESULT = s32(0x8002b601), GAMEDATA_ERROR_ACCESS = s32(0x8002b602),
              GAMEDATA_ERROR_PARAM = s32(0x8002b604), GAMEDATA_ERROR_BROKEN = s32(0x8002b606);
constexpr s32 HDDGAME_ERROR_CBRESULT = s32(0x8002ba01), HDDGAME_ERROR_ACCESS = s32(0x8002ba02),
              HDDGAME_ERROR_PARAM = s32(0x8002ba04), HDDGAME_ERROR_BROKEN = s32(0x8002ba06);

// CellGameDataStatGet / CellHddGameStatGet share this layout.
constexpr u32 kStatGetSize = 0x1448;
constexpr u32 G_FREE = 0x0, G_NEW = 0x4, G_INFO_PATH = 0x8, G_DATA_PATH = 0x427, G_PARAM = 0x860, G_SIZEKB = 0x13FC,
              G_SYSSIZEKB = 0x1400;
// Within the system file param.
constexpr u32 P_TITLE = 0x0, P_TITLE_LANG = 0x80, P_TITLE_ID = 0xA80, P_VERSION = 0xA8C, P_PARENTAL = 0xA94, P_ATTR = 0xA98;

bool host_path(const std::string& guest, fs::path& out) {
  vfs::Resolved r;
  if (!vfs::resolve(guest, r)) return false;
  out = r.host;
  return true;
}

void fill_param(u32 p, const ac4::sfo::Map& sfo) {
  write_cstr(p + P_TITLE, ac4::sfo::get_string(sfo, "TITLE"), 128);
  for (int i = 0; i < 20; ++i)
    write_cstr(p + P_TITLE_LANG + 128 * i, ac4::sfo::get_string(sfo, ac4::strprintf("TITLE_%02d", i)), 128);
  write_cstr(p + P_TITLE_ID, ac4::sfo::get_string(sfo, "TITLE_ID"), 10);
  std::string ver = ac4::sfo::get_string(sfo, "APP_VER", ac4::sfo::get_string(sfo, "VERSION"));
  write_cstr(p + P_VERSION, ver, 6);
  ppu::w32(p + P_PARENTAL, ac4::sfo::get_int(sfo, "PARENTAL_LEVEL"));
  ppu::w32(p + P_ATTR, 0);
}

void apply_param(ac4::sfo::Map& sfo, u32 p) {
  ac4::sfo::set_string(sfo, "TITLE_ID", read_cstr(p + P_TITLE_ID, 10), 16);
  ac4::sfo::set_string(sfo, "TITLE", read_cstr(p + P_TITLE, 128), 128);
  ac4::sfo::set_string(sfo, "VERSION", read_cstr(p + P_VERSION, 6), 8);
  ac4::sfo::set_int(sfo, "PARENTAL_LEVEL", ppu::r32(p + P_PARENTAL));
  for (int i = 0; i < 20; ++i) {
    std::string t = read_cstr(p + P_TITLE_LANG + 128 * i, 128);
    if (!t.empty()) ac4::sfo::set_string(sfo, ac4::strprintf("TITLE_%02d", i), t, 128);
  }
}

// Shared implementation; `hdd` selects the cellHddGameCheck variant (category HG).
s32 check_create(ppu::Ctx& ctx, u32 version, u32 dir_name, u32 func_stat, bool hdd) {
  const s32 E_PARAM = hdd ? HDDGAME_ERROR_PARAM : GAMEDATA_ERROR_PARAM, E_BROKEN = hdd ? HDDGAME_ERROR_BROKEN : GAMEDATA_ERROR_BROKEN,
            E_ACCESS = hdd ? HDDGAME_ERROR_ACCESS : GAMEDATA_ERROR_ACCESS, E_CB = hdd ? HDDGAME_ERROR_CBRESULT : GAMEDATA_ERROR_CBRESULT;
  if (!func_stat || !dir_name) return E_PARAM;
  std::string name = read_cstr(dir_name, 32);
  if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return E_PARAM;
  std::string dir = "/dev_hdd0/game/" + name, usrdir = dir + "/USRDIR";
  fs::path host_dir, host_sfo;
  if (!host_path(dir, host_dir)) return E_ACCESS;
  host_sfo = host_dir / "PARAM.SFO";
  ac4::sfo::Map sfo;
  bool is_new = true;
  std::error_code ec;
  if (fs::exists(host_sfo, ec)) {
    try {
      sfo = ac4::sfo::parse(ac4::read_file(host_sfo));
      is_new = false;
    } catch (...) {
      return E_BROKEN;
    }
    std::string cat = ac4::sfo::get_string(sfo, "CATEGORY");
    if (!hdd && cat != "GD" && cat != "DG") return E_BROKEN;
  }
  hle::StackAlloc sa(ctx, 16 + kStatGetSize + 16);
  u32 result = sa.addr(), get = result + 16, set = get + kStatGetSize;
  ppu::w32(get + G_FREE, 40 * 1024 * 1024 - 256);
  ppu::w32(get + G_NEW, is_new ? 1 : 0);
  write_cstr(get + G_INFO_PATH, dir, 1055);
  write_cstr(get + G_DATA_PATH, usrdir, 1055);
  fill_param(get + G_PARAM, sfo);
  ppu::w32(get + G_SIZEKB, u32(-1));  // CELL_GAMEDATA_SIZEKB_NOTCALC
  ppu::w32(get + G_SYSSIZEKB, 0);
  LOG_I("gamedata", "%s('%s'): %s data", hdd ? "cellHddGameCheck" : "cellGameDataCheckCreate2", name.c_str(),
        is_new ? "new" : "existing");
  hle::call_guest(ctx, func_stat, {result, get, set});
  s32 res = s32(ppu::r32(result));
  LOG_I("gamedata", "  status callback result %d", res);
  if (res == 1) return CELL_OK;  // OK_CANCEL
  if (res != 0) return E_CB;
  u32 set_param = ppu::r32(set);
  fs::path host_usr;
  if (!host_path(usrdir, host_usr)) return E_ACCESS;
  if (is_new) {
    if (!set_param) return E_PARAM;
    ac4::guard_write(host_usr);
    fs::create_directories(host_usr, ec);
    if (ec) return E_ACCESS;
  }
  if (set_param) {
    if (is_new) ac4::sfo::set_string(sfo, "CATEGORY", hdd ? "HG" : "GD", 4);
    apply_param(sfo, set_param);
    ac4::write_file(host_sfo, ac4::sfo::build(sfo));
  }
  return CELL_OK;
}

}  // namespace

void cellGameDataCheckCreate2(ppu::Ctx& ctx) {
  u32 version = u32(ctx.r[3]), dir = u32(ctx.r[4]), func = u32(ctx.r[6]);
  s32 r = version != 0 ? GAMEDATA_ERROR_PARAM : check_create(ctx, version, dir, func, false);
  ctx.r[3] = u64(s64(r));
}

void cellHddGameCheck(ppu::Ctx& ctx) {
  u32 version = u32(ctx.r[3]), dir = u32(ctx.r[4]), func = u32(ctx.r[6]);
  s32 r = check_create(ctx, version, dir, func, true);
  ctx.r[3] = u64(s64(r));
}

// CellSysCacheParam { char cacheId[32]; char getCachePath[1024]; be32 reserved; }
void cellSysCacheMount(ppu::Ctx& ctx) {
  u32 param = u32(ctx.r[3]);
  if (!param) {
    ctx.r[3] = u64(s64(s32(0x8002bc03)));  // CELL_SYSCACHE_ERROR_PARAM
    return;
  }
  std::string id = read_cstr(param, 32);
  write_cstr(param + 32, "/dev_hdd1", 1024);
  fs::path root(cfg().hdd1), marker = root / ".cache_id";
  std::string prev;
  std::error_code ec;
  if (fs::exists(marker, ec)) {
    auto v = ac4::read_file(marker);
    prev.assign(v.begin(), v.end());
  }
  s32 r;
  if (!id.empty() && prev == cfg().title_id + "_" + id) {
    r = 1;  // CELL_SYSCACHE_RET_OK_RELAYED: contents preserved
  } else {
    for (const auto& e : fs::directory_iterator(root, ec))  // project-owned cache directory only
      if (e.path().filename() != ".cache_id") fs::remove_all(e.path(), ec);
    ac4::write_text(marker, cfg().title_id + "_" + id);
    r = 0;  // CELL_SYSCACHE_RET_OK_CLEARED
  }
  LOG_I("gamedata", "cellSysCacheMount('%s') -> %s", id.c_str(), r ? "relayed (kept)" : "cleared");
  ctx.r[3] = u64(r);
}

}  // namespace rt::game_data
