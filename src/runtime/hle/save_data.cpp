// HLE (cellSysutil): cellSaveData*2 — the system save data utility.
//
// Saves live in the console layout under /dev_hdd0/home/00000001/savedata/<DIR> (the project-owned
// local\hdd0): PARAM.SFO plus the game's files. Files are stored unencrypted, so saves are not
// interchangeable with a real console's (no PARAM.PFD). The callback protocol follows the SDK:
// list/fixed callback -> system list or confirmation -> status callback -> file callbacks.
//
// Writes are transactional: file operations act on an in-memory copy, and only a sequence that
// the game finishes successfully is committed, by writing a complete new directory and swapping it
// in (an interrupted commit is repaired the next time the directory is used).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "common/file.h"
#include "common/sfo.h"
#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/sysdialog.h"
#include "runtime/vfs.h"

namespace rt::save_data {
namespace fs = std::filesystem;
namespace {

constexpr s32 SAVEDATA_ERROR_CBRESULT = s32(0x8002b401), SAVEDATA_ERROR_ACCESS_ERROR = s32(0x8002b402),
              SAVEDATA_ERROR_PARAM = s32(0x8002b404), SAVEDATA_ERROR_BROKEN = s32(0x8002b406),
              SAVEDATA_ERROR_FAILURE = s32(0x8002b407), SAVEDATA_ERROR_BUSY = s32(0x8002b408);
constexpr s32 RET_CANCEL = 1;  // CELL_SAVEDATA_RET_CANCEL
constexpr s32 CB_OK_LAST_NOCONFIRM = 2, CB_OK_LAST = 1, CB_OK_NEXT = 0, CB_ERR_NOSPACE = -1, CB_ERR_BROKEN = -3,
              CB_ERR_NODATA = -4, CB_ERR_INVALID = -5;
constexpr u32 FOCUS_DIRNAME = 0, FOCUS_LISTHEAD = 1, FOCUS_LISTTAIL = 2, FOCUS_LATEST = 3, FOCUS_OLDEST = 4,
              FOCUS_NEWDATA = 5;
constexpr u32 FILEOP_READ = 0, FILEOP_WRITE = 1, FILEOP_DELETE = 2, FILEOP_WRITE_NOTRUNC = 3;
constexpr u32 FILETYPE_SECURE = 0, FILETYPE_NORMAL = 1, FILETYPE_ICON0 = 2, FILETYPE_ICON1 = 3, FILETYPE_PIC1 = 4,
              FILETYPE_SND0 = 5;
constexpr u32 RECREATE_NO = 0, RECREATE_NO_NOBROKEN = 1, RECREATE_YES = 2, RECREATE_YES_RESET_OWNER = 3,
              RECREATE_MASK = 0xfffeffff;
constexpr u32 OPTION_NONE = 0, OPTION_NOCONFIRM = 1;
constexpr u32 ERRDIALOG_NONE = 0;
constexpr u32 SYS_SIZE_KB = 35;  // reported size of the system files (PARAM.SFO etc.)

// ---- guest structures (SDK layouts) ----
struct SetList { be32 sortType, sortOrder, dirNamePrefix, reserved; };
struct SetBuf { be32 dirListMax, fileListMax, reserved[6], bufSize, buf; };
struct NewDataIcon { be32 title, iconBufSize, iconBuf, reserved; };
struct ListNewData { be32 iconPosition, dirName, icon, reserved; };
struct DirList { char dirName[32]; char listParam[8]; char reserved[8]; };
struct ListGet { be32 dirNum, dirListNum, dirList; char reserved[64]; };
struct ListSet { be32 focusPosition, focusDirName, fixedListNum, fixedList, newData, reserved; };
struct FixedSet { be32 dirName, newIcon, option; };
struct SysParam {
  char title[128];
  char subTitle[128];
  char detail[1024];
  be32 attribute, parental_level;
  char listParam[8];
  char reserved[256];
};
struct DirStat { bes64 atime, mtime, ctime; char dirName[32]; };
struct FileStat {
  be32 fileType;
  char reserved1[4];
  be64 size;
  bes64 atime, mtime, ctime;
  char fileName[13];
  char reserved2[3];
};
struct StatGet {
  bes32 hddFreeSizeKB;
  be32 isNewData;
  DirStat dir;
  SysParam getParam;
  be32 bind;
  bes32 sizeKB, sysSizeKB;
  be32 fileNum, fileListNum, fileList;
  char reserved[64];
};
struct StatSet { be32 setParam, reCreateMode, indicator; };
struct FileGet { be32 excSize; char reserved[64]; };
struct FileSet {
  be32 fileOperation, reserved, fileType;
  u8 secureFileId[16];
  be32 fileName, fileOffset, fileSize, fileBufSize, fileBuf;
};
struct CBResult { bes32 result; be32 progressBarInc; bes32 errNeedSizeKB; be32 invalidMsg, userdata; };
static_assert(sizeof(SetBuf) == 40 && sizeof(DirList) == 48 && sizeof(ListGet) == 76 && sizeof(ListSet) == 24);
static_assert(sizeof(SysParam) == 1552 && sizeof(DirStat) == 56 && sizeof(FileStat) == 56 && sizeof(StatGet) == 1704);
static_assert(sizeof(FileSet) == 48 && sizeof(CBResult) == 20 && sizeof(FileGet) == 68);

// The structures handed to the callbacks, allocated on the caller's stack for one operation.
struct Frame {
  CBResult result;
  u8 pad0[4];
  ListGet listGet;
  ListSet listSet;
  FixedSet fixedSet;
  StatGet statGet;
  StatSet statSet;
  FileGet fileGet;
  FileSet fileSet;
};

enum class Op { ListSave, ListLoad, FixedSave, FixedLoad, AutoSave, AutoLoad };
bool is_save(Op op) { return op == Op::ListSave || op == Op::FixedSave || op == Op::AutoSave; }
const char* op_name(Op op) {
  switch (op) {
    case Op::ListSave: return "cellSaveDataListSave2";
    case Op::ListLoad: return "cellSaveDataListLoad2";
    case Op::FixedSave: return "cellSaveDataFixedSave2";
    case Op::FixedLoad: return "cellSaveDataFixedLoad2";
    case Op::AutoSave: return "cellSaveDataAutoSave2";
    case Op::AutoLoad: return "cellSaveDataAutoLoad2";
  }
  return "?";
}

std::mutex g_busy;  // one save data operation at a time (CELL_SAVEDATA_ERROR_BUSY otherwise)

std::string fixed_str(const char* p, size_t n) { return std::string(p, strnlen(p, n)); }
void put_str(char* dst, size_t n, const std::string& s) {
  std::memset(dst, 0, n);
  std::memcpy(dst, s.data(), std::min(s.size(), n - 1));
}

// Directory names: 1..31 characters of A-Z, 0-9, '-', '_'.
bool valid_dir_name(const std::string& s) {
  if (s.empty() || s.size() > 31) return false;
  for (char c : s)
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  return true;
}
// Game file names: "NAME.EXT" with up to 8 + 3 characters of A-Z, 0-9, '-', '_'; system names excluded.
bool valid_file_name(const std::string& s) {
  static const std::set<std::string> reserved = {"PARAM.SFO", "PARAM.PFD", "ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3"};
  if (s.empty() || s.size() > 12 || reserved.count(s)) return false;
  size_t dot = s.find('.');
  if (dot != std::string::npos && (s.find('.', dot + 1) != std::string::npos || dot == 0 || dot > 8 || s.size() - dot - 1 > 3))
    return false;
  if (dot == std::string::npos && s.size() > 8) return false;
  for (char c : s)
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  return true;
}
const char* content_name(u32 type) {
  switch (type) {
    case FILETYPE_ICON0: return "ICON0.PNG";
    case FILETYPE_ICON1: return "ICON1.PAM";
    case FILETYPE_PIC1: return "PIC1.PNG";
    case FILETYPE_SND0: return "SND0.AT3";
  }
  return nullptr;
}

// Last modification time of a host file or directory, in seconds since 1970.
s64 host_mtime(const fs::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
  u64 t = (u64(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
  return s64(t / 10000000ull) - 11644473600ll;
}

s32 free_kb(const fs::path& p) {
  std::error_code ec;
  auto si = fs::space(p, ec);
  if (ec) return 0;
  return s32(std::min<u64>(si.available / 1024, 0x7fffffff));
}

fs::path savedata_root() {
  vfs::Resolved r;
  if (!vfs::resolve("/dev_hdd0/home/00000001/savedata", r)) return {};
  return r.host;
}

// Completes or discards a commit that was interrupted (see commit()).
void repair(const fs::path& root, const std::string& dir) {
  std::error_code ec;
  fs::path cur = root / dir, staged = root / ("." + dir + ".new"), old = root / ("." + dir + ".old");
  bool staged_complete = fs::exists(staged / "PARAM.SFO", ec);  // PARAM.SFO is written last
  if (!fs::exists(cur, ec) && staged_complete) {
    ac4::guard_write(cur);
    fs::rename(staged, cur, ec);
    LOG_W("savedata", "%s: completed an interrupted commit", dir.c_str());
  }
  for (const fs::path& p : {staged, old})
    if (fs::exists(p, ec)) {
      ac4::guard_write(p);
      fs::remove_all(p, ec);
    }
}

struct Entry {
  std::string dir, list_param, title, subtitle, detail;
  s64 atime = 0, mtime = 0, ctime = 0;
};

// Save directories whose names start with one of the '|'-separated prefixes.
std::vector<Entry> enumerate(const fs::path& root, const std::string& prefixes) {
  std::vector<std::string> pre;
  for (size_t p = 0;;) {
    size_t q = prefixes.find('|', p);
    pre.push_back(prefixes.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  std::vector<Entry> out;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(root, ec)) {
    std::string name = d.path().filename().string();
    if (!d.is_directory(ec) || !valid_dir_name(name)) continue;
    if (std::none_of(pre.begin(), pre.end(), [&](const std::string& p) { return name.compare(0, p.size(), p) == 0; })) continue;
    repair(root, name);
    ac4::sfo::Map sfo;
    try {
      sfo = ac4::sfo::parse(ac4::read_file(d.path() / "PARAM.SFO"));
    } catch (...) {
      LOG_W("savedata", "%s: no readable PARAM.SFO; not listed", name.c_str());
      continue;
    }
    Entry e;
    e.dir = name;
    e.list_param = ac4::sfo::get_string(sfo, "SAVEDATA_LIST_PARAM");
    e.title = ac4::sfo::get_string(sfo, "TITLE");
    e.subtitle = ac4::sfo::get_string(sfo, "SUB_TITLE");
    e.detail = ac4::sfo::get_string(sfo, "DETAIL");
    e.mtime = e.atime = e.ctime = host_mtime(d.path());
    out.push_back(std::move(e));
  }
  return out;
}

void sort_entries(std::vector<Entry>& v, u32 type, u32 order) {
  auto less = [type](const Entry& a, const Entry& b) {
    if (type == 1) return a.subtitle != b.subtitle ? a.subtitle < b.subtitle : a.mtime < b.mtime;  // SORTTYPE_SUBTITLE
    return a.mtime != b.mtime ? a.mtime < b.mtime : a.subtitle < b.subtitle;                       // MODIFIEDTIME
  };
  std::stable_sort(v.begin(), v.end(), less);
  if (order == 0) std::reverse(v.begin(), v.end());  // SORTORDER_DESCENT
}

// The working copy of one save directory.
struct Session {
  fs::path root, host;
  std::string dir;
  bool is_new = true;
  ac4::sfo::Map sfo;
  std::map<std::string, std::vector<u8>> files;  // everything except PARAM.SFO
  std::map<std::string, s64> mtimes;
  bool modified = false;

  bool load() {
    host = root / dir;
    repair(root, dir);
    std::error_code ec;
    if (!fs::exists(host / "PARAM.SFO", ec)) return true;
    try {
      sfo = ac4::sfo::parse(ac4::read_file(host / "PARAM.SFO"));
    } catch (...) {
      return false;
    }
    is_new = false;
    for (const auto& f : fs::directory_iterator(host, ec)) {
      std::string name = f.path().filename().string();
      if (!f.is_regular_file(ec) || name == "PARAM.SFO") continue;
      files[name] = ac4::read_file(f.path());
      mtimes[name] = host_mtime(f.path());
    }
    return true;
  }

  // Protected (secure) files are listed in PARAM.SFO the way RPCS3 does it, so saves move
  // between the two: an integer "*<file name>" per file, 1 if protected.
  bool secure(const std::string& name) const {
    return sfo.count("*" + name) && ac4::sfo::get_int(sfo, "*" + name) != 0;
  }
  void set_secure(const std::string& name, bool on) { ac4::sfo::set_int(sfo, "*" + name, on ? 1 : 0); }
  void forget_file(const std::string& name) { sfo.erase("*" + name); }
  void forget_all_files() {
    for (auto it = sfo.begin(); it != sfo.end();) it = it->first.size() > 1 && it->first[0] == '*' ? sfo.erase(it) : std::next(it);
  }

  // Writes the complete new directory next to the old one, then swaps them.
  bool commit() {
    fs::path staged = root / ("." + dir + ".new"), old = root / ("." + dir + ".old");
    std::error_code ec;
    ac4::guard_write(staged);
    fs::remove_all(staged, ec);
    fs::create_directories(staged, ec);
    if (ec) return false;
    try {
      for (const auto& [name, data] : files) ac4::write_file(staged / name, data);
      ac4::write_file(staged / "PARAM.SFO", ac4::sfo::build(sfo));  // last: marks the copy complete
    } catch (const std::exception& e) {
      LOG_E("savedata", "%s: writing failed: %s", dir.c_str(), e.what());
      fs::remove_all(staged, ec);
      return false;
    }
    if (fs::exists(host, ec)) {
      ac4::guard_write(old);
      fs::remove_all(old, ec);
      fs::rename(host, old, ec);
      if (ec) {
        LOG_E("savedata", "%s: cannot replace the old copy: %s", dir.c_str(), ec.message().c_str());
        fs::remove_all(staged, ec);
        return false;
      }
    }
    fs::rename(staged, host, ec);
    if (ec) {
      LOG_E("savedata", "%s: cannot commit: %s", dir.c_str(), ec.message().c_str());
      return false;  // repair() completes it on the next access
    }
    fs::remove_all(old, ec);
    return true;
  }
};

std::string cb_result_text(s32 r, const CBResult& res) {
  switch (r) {
    case CB_ERR_NOSPACE:
      return ac4::strprintf("There is not enough free space. %d KB more is required.", s32(res.errNeedSizeKB));
    case CB_ERR_BROKEN: return "The saved data is corrupted.";
    case CB_ERR_NODATA: return "There is no saved data.";
    case CB_ERR_INVALID: return res.invalidMsg ? read_cstr(res.invalidMsg, 256) : std::string("The saved data cannot be used.");
    default: return "An error occurred while processing the saved data.";
  }
}

// A callback reported an error: the system shows it (when asked to) and fails the call.
s32 callback_error(Op op, const char* which, const CBResult& res, bool dialog) {
  s32 r = res.result;
  LOG_W("savedata", "%s: %s callback returned %d", op_name(op), which, r);
  if (r < CB_ERR_INVALID || r > CB_OK_LAST_NOCONFIRM) return SAVEDATA_ERROR_PARAM;
  if (dialog) sysdlg::message(cb_result_text(r, res));
  return SAVEDATA_ERROR_CBRESULT;
}

void reset_result(Frame* f, u32 userdata) {
  std::memset(&f->result, 0, sizeof f->result);
  f->result.userdata = userdata;
}

struct Args {
  Op op = Op::AutoLoad;
  u32 set_list = 0, set_buf = 0, func_list = 0, func_fixed = 0, func_stat = 0, func_file = 0, userdata = 0;
  std::string dir_name;  // Auto operations
  u32 err_dialog = ERRDIALOG_NONE;
  u32 version = 0;
};

s32 run(ppu::Ctx& ctx, const Args& a) {
  std::unique_lock busy(g_busy, std::try_to_lock);
  if (!busy) return SAVEDATA_ERROR_BUSY;
  const bool list_op = a.op == Op::ListSave || a.op == Op::ListLoad, fixed_op = a.op == Op::FixedSave || a.op == Op::FixedLoad;
  const bool dialogs = list_op || fixed_op || a.err_dialog != ERRDIALOG_NONE;
  if (a.version > 1 || !a.set_buf || !a.func_stat || (list_op && (!a.set_list || !a.func_list)) || (fixed_op && (!a.set_list || !a.func_fixed)))
    return SAVEDATA_ERROR_PARAM;
  SetBuf* buf = gp<SetBuf>(a.set_buf);
  if (buf->dirListMax > 2048 || (buf->bufSize && !buf->buf)) return SAVEDATA_ERROR_PARAM;
  fs::path root = savedata_root();
  std::error_code ec;
  if (root.empty()) return SAVEDATA_ERROR_ACCESS_ERROR;
  ac4::guard_write(root);
  fs::create_directories(root, ec);

  hle::StackAlloc sa(ctx, sizeof(Frame));
  const u32 fa = sa.addr();
  Frame* f = gp<Frame>(fa);
  const u32 a_result = fa + offsetof(Frame, result), a_listGet = fa + offsetof(Frame, listGet),
            a_listSet = fa + offsetof(Frame, listSet), a_fixedSet = fa + offsetof(Frame, fixedSet),
            a_statGet = fa + offsetof(Frame, statGet), a_statSet = fa + offsetof(Frame, statSet),
            a_fileGet = fa + offsetof(Frame, fileGet), a_fileSet = fa + offsetof(Frame, fileSet);

  std::string dir = a.dir_name;
  if (list_op || fixed_op) {
    SetList* sl = gp<SetList>(a.set_list);
    std::string prefix = sl->dirNamePrefix ? read_cstr(sl->dirNamePrefix, 256) : std::string();
    std::vector<Entry> entries = enumerate(root, prefix);
    sort_entries(entries, sl->sortType, sl->sortOrder);
    f->listGet.dirNum = u32(entries.size());
    f->listGet.dirListNum = std::min<u32>(u32(entries.size()), buf->dirListMax);
    f->listGet.dirList = buf->buf;
    if (buf->buf && buf->bufSize) std::memset(gp<u8>(buf->buf), 0, buf->bufSize);
    u32 room = buf->buf ? u32(buf->bufSize / sizeof(DirList)) : 0;
    for (u32 i = 0; i < f->listGet.dirListNum && i < room; ++i) {
      DirList* d = gp<DirList>(buf->buf + i * u32(sizeof(DirList)));
      put_str(d->dirName, sizeof d->dirName, entries[i].dir);
      std::memcpy(d->listParam, entries[i].list_param.data(), std::min<size_t>(entries[i].list_param.size(), 8));
    }
    LOG_I("savedata", "%s: prefix '%s', %zu save(s) found", op_name(a.op), prefix.c_str(), entries.size());

    if (list_op) {
      reset_result(f, a.userdata);
      f->listSet.focusPosition = FOCUS_LISTHEAD;
      hle::call_guest(ctx, a.func_list, {a_result, a_listGet, a_listSet});
      s32 r = f->result.result;
      if (r == CB_OK_LAST || r == CB_OK_LAST_NOCONFIRM) return CELL_OK;
      if (r != CB_OK_NEXT) return callback_error(a.op, "list", f->result, true);
      const ListSet& ls = f->listSet;
      if (ls.fixedListNum > 2048 || (ls.fixedListNum && !ls.fixedList)) return SAVEDATA_ERROR_PARAM;
      std::set<std::string> shown;
      for (u32 i = 0; i < ls.fixedListNum; ++i) shown.insert(fixed_str(gp<DirList>(ls.fixedList + i * 48)->dirName, 32));
      std::vector<Entry> visible;
      for (const Entry& e : entries)
        if (shown.count(e.dir)) visible.push_back(e);
      std::string new_dir, new_title = "New Saved Data";
      bool new_at_head = true;
      if (ls.newData) {
        const ListNewData* nd = gp<ListNewData>(ls.newData);
        if (nd->iconPosition > 1 || !nd->dirName) return SAVEDATA_ERROR_PARAM;
        new_dir = read_cstr(nd->dirName, 32);
        if (!valid_dir_name(new_dir)) return SAVEDATA_ERROR_PARAM;
        if (nd->icon && gp<NewDataIcon>(nd->icon)->title) new_title = read_cstr(gp<NewDataIcon>(nd->icon)->title, 128);
        new_at_head = nd->iconPosition == 0;
      }
      std::vector<sysdlg::ListItem> items;
      std::vector<int> map;  // item -> entry index (-1 = new data)
      if (ls.newData && new_at_head) items.push_back({new_title, "", "", 0, true}), map.push_back(-1);
      for (size_t i = 0; i < visible.size(); ++i)
        items.push_back({visible[i].title, visible[i].subtitle, visible[i].detail, visible[i].mtime, false}), map.push_back(int(i));
      if (ls.newData && !new_at_head) items.push_back({new_title, "", "", 0, true}), map.push_back(-1);
      int focus = 0;
      switch (u32(ls.focusPosition)) {
        case FOCUS_DIRNAME: {
          std::string want = ls.focusDirName ? read_cstr(ls.focusDirName, 32) : std::string();
          for (size_t i = 0; i < map.size(); ++i)
            if (map[i] >= 0 && visible[map[i]].dir == want) focus = int(i);
          break;
        }
        case FOCUS_LISTHEAD: focus = 0; break;
        case FOCUS_LISTTAIL: focus = int(items.size()) - 1; break;
        case FOCUS_LATEST:
        case FOCUS_OLDEST: {
          s64 best = 0;
          bool have = false;
          for (size_t i = 0; i < map.size(); ++i) {
            if (map[i] < 0) continue;
            s64 t = visible[map[i]].mtime;
            if (!have || (ls.focusPosition == FOCUS_LATEST ? t > best : t < best)) best = t, focus = int(i), have = true;
          }
          break;
        }
        case FOCUS_NEWDATA:
          if (!ls.newData) return SAVEDATA_ERROR_PARAM;
          for (size_t i = 0; i < map.size(); ++i)
            if (map[i] < 0) focus = int(i);
          break;
        default: return SAVEDATA_ERROR_PARAM;
      }
      if (items.empty()) {
        sysdlg::message("There is no saved data.");
        LOG_I("savedata", "%s: nothing to list", op_name(a.op));
        return RET_CANCEL;
      }
      for (;;) {
        int pick = sysdlg::choose(is_save(a.op) ? "Save" : "Load", items, std::clamp(focus, 0, int(items.size()) - 1));
        if (pick < 0) {
          LOG_I("savedata", "%s: cancelled in the list", op_name(a.op));
          return RET_CANCEL;
        }
        bool existing = map[pick] >= 0;
        dir = existing ? visible[map[pick]].dir : new_dir;
        if (is_save(a.op) && existing && !sysdlg::confirm("Do you want to overwrite this saved data?\n\n" + items[pick].title + "\n" + items[pick].subtitle)) {
          focus = pick;
          continue;
        }
        break;
      }
    } else {
      reset_result(f, a.userdata);
      hle::call_guest(ctx, a.func_fixed, {a_result, a_listGet, a_fixedSet});
      s32 r = f->result.result;
      if (r == CB_OK_LAST) return CELL_OK;
      if (r != CB_OK_NEXT) return callback_error(a.op, "fixed", f->result, true);
      if (!f->fixedSet.dirName) return SAVEDATA_ERROR_PARAM;
      dir = read_cstr(f->fixedSet.dirName, 32);
      if (!valid_dir_name(dir)) return SAVEDATA_ERROR_PARAM;
      if (f->fixedSet.option != OPTION_NONE && f->fixedSet.option != OPTION_NOCONFIRM) return SAVEDATA_ERROR_PARAM;
      if (f->fixedSet.option == OPTION_NONE) {
        bool exists = std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.dir == dir; });
        std::string q = is_save(a.op) ? (exists ? "Do you want to overwrite this saved data?" : "Do you want to save?")
                                      : "Do you want to load this saved data?";
        if (!sysdlg::confirm(q)) {
          LOG_I("savedata", "%s: '%s' declined in the confirmation", op_name(a.op), dir.c_str());
          return RET_CANCEL;
        }
      }
    }
  }
  if (!valid_dir_name(dir)) return SAVEDATA_ERROR_PARAM;

  Session s;
  s.root = root;
  s.dir = dir;
  if (!s.load()) {
    LOG_W("savedata", "%s: '%s' has a damaged PARAM.SFO", op_name(a.op), dir.c_str());
    if (dialogs) sysdlg::message("The saved data is corrupted.");
    return SAVEDATA_ERROR_BROKEN;
  }

  // ---- status ----
  StatGet& sg = f->statGet;
  std::memset(&sg, 0, sizeof sg);
  sg.hddFreeSizeKB = free_kb(root);
  sg.isNewData = s.is_new ? 1 : 0;
  put_str(sg.dir.dirName, sizeof sg.dir.dirName, dir);
  if (!s.is_new) {
    s64 t = host_mtime(s.host);
    sg.dir.atime = t;
    sg.dir.mtime = t;
    sg.dir.ctime = t;
    put_str(sg.getParam.title, 128, ac4::sfo::get_string(s.sfo, "TITLE"));
    put_str(sg.getParam.subTitle, 128, ac4::sfo::get_string(s.sfo, "SUB_TITLE"));
    put_str(sg.getParam.detail, 1024, ac4::sfo::get_string(s.sfo, "DETAIL"));
    sg.getParam.attribute = ac4::sfo::get_int(s.sfo, "ATTRIBUTE");
    sg.getParam.parental_level = ac4::sfo::get_int(s.sfo, "PARENTAL_LEVEL");
    std::string lp = ac4::sfo::get_string(s.sfo, "SAVEDATA_LIST_PARAM");
    std::memcpy(sg.getParam.listParam, lp.data(), std::min<size_t>(lp.size(), 8));
  }
  sg.bind = 0;  // created by this installation: no binding mismatch
  sg.fileList = buf->buf;
  if (buf->buf && buf->bufSize) std::memset(gp<u8>(buf->buf), 0, buf->bufSize);
  u64 bytes = 0;
  u32 room = buf->buf ? u32(buf->bufSize / sizeof(FileStat)) : 0;
  for (const auto& [name, data] : s.files) {
    sg.fileNum += 1;
    bytes += (data.size() + 1023) & ~u64(1023);
    if (sg.fileListNum >= buf->fileListMax || sg.fileListNum >= room) continue;
    FileStat* fst = gp<FileStat>(buf->buf + sg.fileListNum * u32(sizeof(FileStat)));
    u32 type = s.secure(name) ? FILETYPE_SECURE : FILETYPE_NORMAL;
    for (u32 t = FILETYPE_ICON0; t <= FILETYPE_SND0; ++t)
      if (name == content_name(t)) type = t;
    fst->fileType = type;
    fst->size = data.size();
    fst->atime = fst->mtime = fst->ctime = s.mtimes[name];
    put_str(fst->fileName, 13, name);
    sg.fileListNum += 1;
  }
  sg.sysSizeKB = s32(SYS_SIZE_KB);
  sg.sizeKB = s.is_new ? 0 : s32(bytes / 1024 + SYS_SIZE_KB);
  LOG_I("savedata", "%s: '%s' (%s, %u file(s), %d KB)", op_name(a.op), dir.c_str(), s.is_new ? "new" : "existing", u32(sg.fileNum),
        s32(sg.sizeKB));
  std::memset(&f->statSet, 0, sizeof f->statSet);
  reset_result(f, a.userdata);
  hle::call_guest(ctx, a.func_stat, {a_result, a_statGet, a_statSet});
  {
    s32 r = f->result.result;
    if (r == CB_OK_LAST) return CELL_OK;
    if (r != CB_OK_NEXT) return callback_error(a.op, "status", f->result, dialogs);
  }
  const StatSet& ss = f->statSet;
  if (ss.setParam) {
    const SysParam* p = gp<SysParam>(ss.setParam);
    if (p->attribute > 1 || p->parental_level > 11) return SAVEDATA_ERROR_PARAM;
    std::string lp = fixed_str(p->listParam, 8);
    for (char c : lp)
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return SAVEDATA_ERROR_PARAM;
    ac4::sfo::set_string(s.sfo, "CATEGORY", "SD", 4);
    ac4::sfo::set_string(s.sfo, "TITLE", fixed_str(p->title, 128), 128);
    ac4::sfo::set_string(s.sfo, "SUB_TITLE", fixed_str(p->subTitle, 128), 128);
    ac4::sfo::set_string(s.sfo, "DETAIL", fixed_str(p->detail, 1024), 1024);
    ac4::sfo::set_int(s.sfo, "ATTRIBUTE", p->attribute);
    ac4::sfo::set_int(s.sfo, "PARENTAL_LEVEL", p->parental_level);
    ac4::sfo::set_string(s.sfo, "SAVEDATA_DIRECTORY", dir, 64);
    ac4::sfo::set_string(s.sfo, "SAVEDATA_LIST_PARAM", lp, 8);
    s.modified = true;
  } else if (s.is_new) {
    LOG_W("savedata", "%s: new data '%s' without parameters", op_name(a.op), dir.c_str());
    return SAVEDATA_ERROR_PARAM;
  }
  switch (u32(ss.reCreateMode) & RECREATE_MASK) {
    case RECREATE_NO:
    case RECREATE_NO_NOBROKEN: break;
    case RECREATE_YES:
    case RECREATE_YES_RESET_OWNER:
      if (!ss.setParam) return SAVEDATA_ERROR_PARAM;
      s.files.clear();
      s.mtimes.clear();
      s.forget_all_files();
      s.modified = true;
      LOG_I("savedata", "%s: '%s' recreated", op_name(a.op), dir.c_str());
      break;
    default: return SAVEDATA_ERROR_PARAM;
  }

  // ---- files ----
  f->fileGet.excSize = 0;
  s32 status = CELL_OK;
  bool finished = !a.func_file;
  while (a.func_file) {
    std::memset(&f->fileSet, 0, sizeof f->fileSet);
    std::memset(f->fileGet.reserved, 0, sizeof f->fileGet.reserved);
    reset_result(f, a.userdata);
    hle::call_guest(ctx, a.func_file, {a_result, a_fileGet, a_fileSet});
    s32 r = f->result.result;
    if (r == CB_OK_LAST || r == CB_OK_LAST_NOCONFIRM) {
      finished = true;
      break;
    }
    if (r != CB_OK_NEXT) {
      status = callback_error(a.op, "file", f->result, dialogs);
      break;
    }
    const FileSet& fset = f->fileSet;
    std::string name;
    if (fset.fileType == FILETYPE_SECURE || fset.fileType == FILETYPE_NORMAL) {
      name = fset.fileName ? read_cstr(fset.fileName, 13) : std::string();
      if (!valid_file_name(name)) {
        LOG_W("savedata", "%s: invalid file name '%s'", op_name(a.op), name.c_str());
        status = SAVEDATA_ERROR_PARAM;
        break;
      }
    } else if (const char* n = content_name(fset.fileType)) {
      name = n;
    } else {
      status = SAVEDATA_ERROR_PARAM;
      break;
    }
    u32 op = fset.fileOperation, off = fset.fileOffset, size = fset.fileSize;
    if (op != FILEOP_DELETE && (fset.fileBufSize < size || (!fset.fileBuf && fset.fileBufSize))) {
      status = SAVEDATA_ERROR_PARAM;
      break;
    }
    if (op == FILEOP_READ) {
      auto it = s.files.find(name);
      if (it == s.files.end() || off >= it->second.size()) {
        LOG_W("savedata", "%s: '%s/%s' cannot be read at offset %u", op_name(a.op), dir.c_str(), name.c_str(), off);
        status = SAVEDATA_ERROR_FAILURE;
        break;
      }
      u32 n = u32(std::min<u64>(size, it->second.size() - off));
      std::memcpy(gp<u8>(fset.fileBuf), it->second.data() + off, n);
      f->fileGet.excSize = n;
      LOG_I("savedata", "  read %s: %u byte(s) at %u", name.c_str(), n, off);
    } else if (op == FILEOP_WRITE || op == FILEOP_WRITE_NOTRUNC) {
      if (!is_save(a.op)) LOG_W("savedata", "%s: file written during a load operation", op_name(a.op));
      std::vector<u8>& data = s.files[name];
      if (data.size() < u64(off) + size) data.resize(u64(off) + size);
      if (size) std::memcpy(data.data() + off, gp<u8>(fset.fileBuf), size);
      if (op == FILEOP_WRITE) data.resize(u64(off) + size);
      s.set_secure(name, fset.fileType == FILETYPE_SECURE);  // content files (icons) are not protected
      f->fileGet.excSize = size;
      s.modified = true;
      LOG_I("savedata", "  write %s: %u byte(s) at %u%s", name.c_str(), size, off, op == FILEOP_WRITE ? "" : " (no truncation)");
    } else if (op == FILEOP_DELETE) {
      if (!s.files.erase(name)) {
        status = SAVEDATA_ERROR_FAILURE;
        break;
      }
      s.forget_file(name);
      f->fileGet.excSize = 0;
      s.modified = true;
      LOG_I("savedata", "  delete %s", name.c_str());
    } else {
      status = SAVEDATA_ERROR_PARAM;
      break;
    }
  }
  if (!finished) {
    LOG_W("savedata", "%s: '%s' not completed (0x%08x); nothing written", op_name(a.op), dir.c_str(), u32(status));
    return status;
  }
  if (s.modified) {
    if (!s.commit()) {
      if (dialogs) sysdlg::message("The saved data could not be written.");
      return SAVEDATA_ERROR_ACCESS_ERROR;
    }
    LOG_I("savedata", "%s: '%s' committed (%zu file(s))", op_name(a.op), dir.c_str(), s.files.size());
  }
  return CELL_OK;
}

}  // namespace

namespace {
// List/Fixed: (version, setList, setBuf, funcList|funcFixed, funcStat, funcFile, container, userdata);
// the SDK 1.x forms (no "2") take the same arguments without userdata.
void list_or_fixed(ppu::Ctx& ctx, Op op, bool has_userdata = true) {
  Args a;
  a.op = op;
  a.version = u32(ctx.r[3]);
  a.set_list = u32(ctx.r[4]);
  a.set_buf = u32(ctx.r[5]);
  (op == Op::ListSave || op == Op::ListLoad ? a.func_list : a.func_fixed) = u32(ctx.r[6]);
  a.func_stat = u32(ctx.r[7]);
  a.func_file = u32(ctx.r[8]);
  a.userdata = has_userdata ? u32(ctx.r[10]) : 0;
  ctx.r[3] = u64(s64(run(ctx, a)));
}
// Auto: (version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata); the SDK 1.x
// forms take the same arguments without userdata.
void automatic(ppu::Ctx& ctx, Op op, bool has_userdata = true) {
  Args a;
  a.op = op;
  a.version = u32(ctx.r[3]);
  a.dir_name = ctx.r[4] ? read_cstr(u32(ctx.r[4]), 32) : std::string();
  a.err_dialog = u32(ctx.r[5]);
  a.set_buf = u32(ctx.r[6]);
  a.func_stat = u32(ctx.r[7]);
  a.func_file = u32(ctx.r[8]);
  a.userdata = has_userdata ? u32(ctx.r[10]) : 0;
  ctx.r[3] = u64(s64(run(ctx, a)));
}
}  // namespace

// The system's delete list (the SYSTEM menu's DELETE GAME): the title's saves; each deletion is
// confirmed; backing out returns (CELL_OK when something was deleted, else CANCEL).
s32 cellSaveDataDelete2(u32 /*container*/) {
  fs::path root = savedata_root();
  if (root.empty()) return SAVEDATA_ERROR_ACCESS_ERROR;
  bool deleted = false;
  for (;;) {
    std::vector<Entry> entries = enumerate(root, "BLUS30187");
    sort_entries(entries, 0, 0);  // newest first
    if (entries.empty()) {
      if (!deleted) sysdlg::message("There is no saved data.");
      break;
    }
    std::vector<sysdlg::ListItem> items;
    for (const Entry& e : entries) items.push_back({e.title, e.subtitle, e.detail, e.mtime, false});
    int pick = sysdlg::choose("Delete", items, 0);
    if (pick < 0) break;
    const Entry& e = entries[size_t(pick)];
    if (!sysdlg::confirm("Delete this saved data?\n\n" + e.title + "\n" + e.subtitle)) continue;
    std::error_code ec;
    fs::path dir = root / e.dir;
    ac4::guard_write(dir);
    fs::remove_all(dir, ec);
    if (ec) {
      LOG_W("savedata", "delete %s failed: %s", e.dir.c_str(), ec.message().c_str());
      sysdlg::message("The saved data could not be deleted.");
      continue;
    }
    LOG_I("savedata", "deleted %s", e.dir.c_str());
    deleted = true;
  }
  return deleted ? CELL_OK : RET_CANCEL;
}

void cellSaveDataListSave2(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::ListSave); }
void cellSaveDataListLoad2(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::ListLoad); }
void cellSaveDataFixedSave2(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::FixedSave); }
void cellSaveDataFixedLoad2(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::FixedLoad); }
void cellSaveDataAutoSave2(ppu::Ctx& ctx) { automatic(ctx, Op::AutoSave); }
void cellSaveDataAutoLoad2(ppu::Ctx& ctx) { automatic(ctx, Op::AutoLoad); }
void cellSaveDataListSave(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::ListSave, false); }
void cellSaveDataListLoad(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::ListLoad, false); }
void cellSaveDataFixedSave(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::FixedSave, false); }
void cellSaveDataFixedLoad(ppu::Ctx& ctx) { list_or_fixed(ctx, Op::FixedLoad, false); }
void cellSaveDataAutoSave(ppu::Ctx& ctx) { automatic(ctx, Op::AutoSave, false); }
void cellSaveDataAutoLoad(ppu::Ctx& ctx) { automatic(ctx, Op::AutoLoad, false); }

// cellSaveDataDelete(container): the system's delete utility for this title's saves: the user picks
// a save, confirms, it is deleted; repeated until the user backs out (CELL_SAVEDATA_RET_CANCEL).
void cellSaveDataDelete(ppu::Ctx& ctx) {
  std::unique_lock busy(g_busy, std::try_to_lock);
  if (!busy) {
    ctx.r[3] = u64(s64(SAVEDATA_ERROR_BUSY));
    return;
  }
  fs::path root = savedata_root();
  int focus = 0;
  for (;;) {
    std::vector<Entry> entries = enumerate(root, cfg().title_id);
    sort_entries(entries, 0, 0);
    if (entries.empty()) {
      sysdlg::message("There is no saved data.");
      break;
    }
    std::vector<sysdlg::ListItem> items;
    for (const Entry& e : entries) items.push_back({e.title, e.subtitle, e.detail, e.mtime, false});
    int pick = sysdlg::choose("Delete", items, std::clamp(focus, 0, int(items.size()) - 1));
    if (pick < 0) break;
    focus = pick;
    const Entry& e = entries[pick];
    if (!sysdlg::confirm("Do you want to delete this saved data?\n\n" + e.title + "\n" + e.subtitle)) continue;
    std::error_code ec;
    fs::path dir = root / e.dir;
    ac4::guard_write(dir);
    fs::remove_all(dir, ec);
    LOG_I("savedata", "cellSaveDataDelete: deleted %s%s", e.dir.c_str(), ec ? (" (error: " + ec.message() + ")").c_str() : "");
    sysdlg::message(ec ? "The saved data could not be deleted." : "The saved data was deleted.");
  }
  LOG_I("savedata", "cellSaveDataDelete: closed");
  ctx.r[3] = u64(s64(RET_CANCEL));
}

}  // namespace rt::save_data
