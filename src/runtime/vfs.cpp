#include "runtime/vfs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "common/file.h"
#include "runtime/hle.h"
#include "runtime/log.h"

namespace rt::vfs {
namespace fs = std::filesystem;
namespace {

struct Mount {
  std::string prefix;  // e.g. "/dev_bdvd"
  fs::path host;
  bool writable;
  std::vector<std::string> parts;  // the prefix's components
};
std::vector<Mount> g_mounts;
fs::path g_disc, g_overlay;  // test overlay over the disc (vfs.h)
std::mutex g_overlay_mu;
std::set<std::string> g_overlaid;  // logged once each

struct File {
  HANDLE h = INVALID_HANDLE_VALUE;
  std::string path;
  bool dir = false;
  std::vector<std::pair<u8, std::string>> entries;
  size_t next = 0;
  bool append = false;
  std::mutex m;
};
std::mutex g_mu;
std::map<u32, std::shared_ptr<File>> g_files;
u32 g_next_fd = 3;

std::shared_ptr<File> get(u32 fd) {
  std::lock_guard lk(g_mu);
  auto it = g_files.find(fd);
  return it == g_files.end() ? nullptr : it->second;
}

s64 ft_to_unix(const FILETIME& ft) {
  u64 t = (u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return t ? s64((t - 116444736000000000ull) / 10000000ull) : 0;
}

s32 win_err(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return CELL_ENOENT;
    case ERROR_ACCESS_DENIED: return CELL_EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return CELL_EEXIST;
    case ERROR_DIR_NOT_EMPTY: return CELL_ENOTEMPTY;
    case ERROR_DISK_FULL: return CELL_ENOSPC;
    case ERROR_SHARING_VIOLATION: return CELL_EBUSY;
    default: return CELL_EIO;
  }
}

}  // namespace

void init(const std::string& disc, const std::string& hdd0, const std::string& hdd1, const std::string& flash,
          const std::string& saves, const std::string& test_overlay) {
  g_disc = fs::path(disc);
  g_overlay = test_overlay.empty() ? fs::path() : fs::path(test_overlay);
  if (!g_overlay.empty()) LOG_W("vfs", "TEST OVERLAY %s: its files replace disc files (test aid)", test_overlay.c_str());
  g_mounts = {
      {"/dev_bdvd", fs::path(disc), false, {}},
      {"/app_home", fs::path(disc) / "PS3_GAME" / "USRDIR", false, {}},
      {"/dev_hdd0", fs::path(hdd0), true, {}},
      {"/dev_hdd1", fs::path(hdd1), true, {}},
      {"/dev_flash", fs::path(flash), false, {}},
  };
  if (!saves.empty()) {
    g_mounts.push_back({"/dev_hdd0/home/00000001/savedata", fs::path(saves), true, {}});
    fs::create_directories(saves);
  }
  for (Mount& m : g_mounts) {
    for (size_t i = 1; i <= m.prefix.size();) {
      size_t j = std::min(m.prefix.find('/', i), m.prefix.size());
      m.parts.push_back(m.prefix.substr(i, j - i));
      i = j + 1;
    }
  }
  fs::create_directories(hdd0);
  fs::create_directories(hdd1);
}

bool resolve(const std::string& gp, Resolved& out) {
  // Normalise: split on '/', drop empty and '.', resolve '..' without escaping the mount.
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= gp.size()) {
    size_t j = gp.find('/', i);
    if (j == std::string::npos) j = gp.size();
    std::string p = gp.substr(i, j - i);
    if (p == "..") {
      if (parts.size() <= 1) return false;
      parts.pop_back();
    } else if (!p.empty() && p != ".") {
      parts.push_back(p);
    }
    i = j + 1;
  }
  if (parts.empty()) return false;
  const Mount* best = nullptr;  // the longest mount prefix that matches whole components
  for (const auto& m : g_mounts) {
    if (m.parts.size() > parts.size() || (best && m.parts.size() <= best->parts.size())) continue;
    bool match = true;
    for (size_t k = 0; k < m.parts.size() && match; ++k) match = _stricmp(m.parts[k].c_str(), parts[k].c_str()) == 0;
    if (match) best = &m;
  }
  if (!best) return false;
  fs::path h = best->host;
  for (size_t k = best->parts.size(); k < parts.size(); ++k) {
    if (parts[k].find(':') != std::string::npos || parts[k].find('\\') != std::string::npos) return false;
    h /= fs::path(std::u8string(parts[k].begin(), parts[k].end()));  // guest paths are UTF-8
  }
  out = {h, best->writable, best->prefix};
  if (!g_overlay.empty() && !best->writable && (best->prefix == "/dev_bdvd" || best->prefix == "/app_home")) {
    std::error_code ec;
    fs::path rel = h.lexically_relative(g_disc);
    fs::path o = g_overlay / rel;
    if (!rel.empty() && *rel.begin() != ".." && fs::is_regular_file(o, ec)) {
      out.host = o;
      std::lock_guard lk(g_overlay_mu);
      if (g_overlaid.insert(o.string()).second) LOG_W("vfs", "TEST OVERLAY: %s read from %s", gp.c_str(), o.string().c_str());
    }
  }
  return true;
}

static s32 resolve_for(const std::string& path, bool want_write, Resolved& r) {
  if (!resolve(path, r)) return CELL_ENOENT;
  if (want_write && !r.writable) return CELL_EROFS;
  if (want_write) {
    try {
      ac4::guard_write(r.host);
    } catch (...) {
      return CELL_EROFS;
    }
  }
  return CELL_OK;
}

s32 open(const std::string& path, s32 flags, u32* fd) {
  constexpr s32 ACC = 3, WR = 1, RDWR = 2, CREAT = 0x40, EXCL = 0x80, TRUNC = 0x200, APPEND = 0x400;
  bool writing = (flags & ACC) == WR || (flags & ACC) == RDWR || (flags & (CREAT | TRUNC | APPEND));
  Resolved r;
  s32 e = resolve_for(path, writing, r);
  if (e != CELL_OK) {
    LOG_D("vfs", "open('%s', 0x%x) -> 0x%x (resolve)", path.c_str(), flags, u32(e));
    return e;
  }
  DWORD access = GENERIC_READ;
  if ((flags & ACC) == WR) access = GENERIC_WRITE;
  if ((flags & ACC) == RDWR) access = GENERIC_READ | GENERIC_WRITE;
  DWORD disp = OPEN_EXISTING;
  if ((flags & CREAT) && (flags & EXCL)) disp = CREATE_NEW;
  else if ((flags & CREAT) && (flags & TRUNC)) disp = CREATE_ALWAYS;
  else if (flags & CREAT) disp = OPEN_ALWAYS;
  else if (flags & TRUNC) disp = TRUNCATE_EXISTING;
  std::error_code ec;
  if (fs::is_directory(r.host, ec)) return CELL_EISDIR;
  HANDLE h = CreateFileW(r.host.wstring().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    s32 err = win_err(GetLastError());
    LOG_D("vfs", "open('%s', 0x%x) -> 0x%x", path.c_str(), flags, u32(err));
    return err;
  }
  auto f = std::make_shared<File>();
  f->h = h;
  f->path = path;
  f->append = flags & APPEND;
  std::lock_guard lk(g_mu);
  *fd = g_next_fd++;
  g_files[*fd] = f;
  LOG_D("vfs", "open('%s', 0x%x) -> fd %u", path.c_str(), flags, *fd);
  return CELL_OK;
}

s32 close(u32 fd) {
  std::shared_ptr<File> f;
  {
    std::lock_guard lk(g_mu);
    auto it = g_files.find(fd);
    if (it == g_files.end() || it->second->dir) return CELL_EBADF;
    f = it->second;
    g_files.erase(it);
  }
  CloseHandle(f->h);
  return CELL_OK;
}

s32 read(u32 fd, void* buf, u64 n, u64* done) {
  auto f = get(fd);
  if (!f || f->dir) return CELL_EBADF;
  std::lock_guard lk(f->m);
  u64 total = 0;
  while (total < n) {
    DWORD chunk = DWORD(std::min<u64>(n - total, 1u << 30)), got = 0;
    if (!ReadFile(f->h, static_cast<u8*>(buf) + total, chunk, &got, nullptr)) return CELL_EIO;
    total += got;
    if (got < chunk) break;
  }
  if (done) *done = total;
  return CELL_OK;
}

// Positioned I/O must not move the file position. On a synchronous handle ReadFile/WriteFile
// with an OVERLAPPED offset also update the file pointer, so it is saved and restored.
s32 pread(u32 fd, u64 offset, void* buf, u64 n, u64* done) {
  auto f = get(fd);
  if (!f || f->dir) return CELL_EBADF;
  std::lock_guard lk(f->m);
  LARGE_INTEGER cur{};
  SetFilePointerEx(f->h, LARGE_INTEGER{}, &cur, FILE_CURRENT);
  OVERLAPPED ov{};
  ov.Offset = DWORD(offset);
  ov.OffsetHigh = DWORD(offset >> 32);
  DWORD got = 0;
  BOOL ok = ReadFile(f->h, buf, DWORD(std::min<u64>(n, 1u << 30)), &got, &ov);
  DWORD e = ok ? 0 : GetLastError();
  SetFilePointerEx(f->h, cur, nullptr, FILE_BEGIN);
  if (!ok && e != ERROR_HANDLE_EOF) return CELL_EIO;
  if (done) *done = got;
  return CELL_OK;
}

s32 pwrite(u32 fd, u64 offset, const void* buf, u64 n, u64* done) {
  auto f = get(fd);
  if (!f || f->dir) return CELL_EBADF;
  std::lock_guard lk(f->m);
  LARGE_INTEGER cur{};
  SetFilePointerEx(f->h, LARGE_INTEGER{}, &cur, FILE_CURRENT);
  OVERLAPPED ov{};
  ov.Offset = DWORD(offset);
  ov.OffsetHigh = DWORD(offset >> 32);
  DWORD wrote = 0;
  BOOL ok = WriteFile(f->h, buf, DWORD(std::min<u64>(n, 1u << 30)), &wrote, &ov);
  DWORD e = ok ? 0 : GetLastError();
  SetFilePointerEx(f->h, cur, nullptr, FILE_BEGIN);
  if (!ok) return win_err(e);
  if (done) *done = wrote;
  return CELL_OK;
}

s32 write(u32 fd, const void* buf, u64 n, u64* done) {
  auto f = get(fd);
  if (!f || f->dir) return CELL_EBADF;
  std::lock_guard lk(f->m);
  if (f->append) SetFilePointerEx(f->h, LARGE_INTEGER{}, nullptr, FILE_END);
  DWORD wrote = 0;
  if (!WriteFile(f->h, buf, DWORD(n), &wrote, nullptr)) return win_err(GetLastError());
  if (done) *done = wrote;
  return CELL_OK;
}

s32 lseek(u32 fd, s64 off, s32 whence, u64* pos) {
  auto f = get(fd);
  if (!f || f->dir) return CELL_EBADF;
  if (whence < 0 || whence > 2) return CELL_EINVAL;
  LARGE_INTEGER li, np;
  li.QuadPart = off;
  if (!SetFilePointerEx(f->h, li, &np, DWORD(whence))) return CELL_EINVAL;
  if (pos) *pos = u64(np.QuadPart);
  return CELL_OK;
}

static void fill_stat(const WIN32_FILE_ATTRIBUTE_DATA& a, Stat* st) {
  st->is_dir = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
  st->size = st->is_dir ? 0 : (u64(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  st->atime = ft_to_unix(a.ftLastAccessTime);
  st->mtime = ft_to_unix(a.ftLastWriteTime);
  st->ctime = ft_to_unix(a.ftCreationTime);
  st->readonly = a.dwFileAttributes & FILE_ATTRIBUTE_READONLY;
}

s32 fstat(u32 fd, Stat* st) {
  auto f = get(fd);
  if (!f) return CELL_EBADF;
  if (f->dir) return stat(f->path, st);
  BY_HANDLE_FILE_INFORMATION bi{};
  if (!GetFileInformationByHandle(f->h, &bi)) return CELL_EIO;
  WIN32_FILE_ATTRIBUTE_DATA a{bi.dwFileAttributes, bi.ftCreationTime, bi.ftLastAccessTime, bi.ftLastWriteTime, bi.nFileSizeHigh,
                              bi.nFileSizeLow};
  fill_stat(a, st);
  return CELL_OK;
}

s32 stat(const std::string& path, Stat* st) {
  Resolved r;
  if (!resolve(path, r)) {
    // The mount points themselves exist.
    return CELL_ENOENT;
  }
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(r.host.wstring().c_str(), GetFileExInfoStandard, &a)) return win_err(GetLastError());
  fill_stat(a, st);
  if (!r.writable) st->readonly = true;
  return CELL_OK;
}

s32 ftruncate(u32 fd, u64 size) {
  auto f = get(fd);
  if (!f || f->dir) return CELL_EBADF;
  std::lock_guard lk(f->m);
  FILE_END_OF_FILE_INFO eof{};
  eof.EndOfFile.QuadPart = s64(size);
  if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &eof, sizeof eof)) return win_err(GetLastError());
  return CELL_OK;
}

s32 truncate(const std::string& path, u64 size) {
  u32 fd;
  s32 e = open(path, 1, &fd);
  if (e != CELL_OK) return e;
  e = ftruncate(fd, size);
  close(fd);
  return e;
}

s32 opendir(const std::string& path, u32* fd) {
  Resolved r;
  if (!resolve(path, r)) return CELL_ENOENT;
  std::error_code ec;
  if (!fs::exists(r.host, ec)) return CELL_ENOENT;
  if (!fs::is_directory(r.host, ec)) return CELL_ENOTDIR;
  auto f = std::make_shared<File>();
  f->dir = true;
  f->path = path;
  f->entries.push_back({1, "."});
  f->entries.push_back({1, ".."});
  for (const auto& e : fs::directory_iterator(r.host, ec))
    {
    auto u = e.path().filename().u8string();
    f->entries.push_back({u8(e.is_directory() ? 1 : 2), std::string(reinterpret_cast<const char*>(u.data()), u.size())});
  }
  std::lock_guard lk(g_mu);
  *fd = g_next_fd++;
  g_files[*fd] = f;
  return CELL_OK;
}

s32 readdir(u32 fd, bool* got, u8* type, std::string* name) {
  auto f = get(fd);
  if (!f || !f->dir) return CELL_EBADF;
  std::lock_guard lk(f->m);
  if (f->next >= f->entries.size()) {
    *got = false;
    return CELL_OK;
  }
  *got = true;
  *type = f->entries[f->next].first;
  *name = f->entries[f->next].second;
  ++f->next;
  return CELL_OK;
}

s32 closedir(u32 fd) {
  std::lock_guard lk(g_mu);
  auto it = g_files.find(fd);
  if (it == g_files.end() || !it->second->dir) return CELL_EBADF;
  g_files.erase(it);
  return CELL_OK;
}

s32 mkdir(const std::string& path) {
  Resolved r;
  s32 e = resolve_for(path, true, r);
  if (e != CELL_OK) return e;
  std::error_code ec;
  if (fs::exists(r.host, ec)) return CELL_EEXIST;
  if (!fs::exists(r.host.parent_path(), ec)) return CELL_ENOENT;
  if (!fs::create_directory(r.host, ec)) return CELL_EIO;
  return CELL_OK;
}

s32 rmdir(const std::string& path) {
  Resolved r;
  s32 e = resolve_for(path, true, r);
  if (e != CELL_OK) return e;
  if (!RemoveDirectoryW(r.host.wstring().c_str())) return win_err(GetLastError());
  return CELL_OK;
}

s32 unlink(const std::string& path) {
  Resolved r;
  s32 e = resolve_for(path, true, r);
  if (e != CELL_OK) return e;
  std::error_code ec;
  if (fs::is_directory(r.host, ec)) return CELL_EISDIR;
  if (!DeleteFileW(r.host.wstring().c_str())) return win_err(GetLastError());
  return CELL_OK;
}

s32 rename(const std::string& from, const std::string& to) {
  Resolved a, b;
  s32 e = resolve_for(from, true, a);
  if (e != CELL_OK) return e;
  e = resolve_for(to, true, b);
  if (e != CELL_OK) return e;
  if (!MoveFileExW(a.host.wstring().c_str(), b.host.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) return win_err(GetLastError());
  return CELL_OK;
}

s32 utime(const std::string& path, s64, s64) {
  Resolved r;
  return resolve_for(path, true, r);
}

std::string fd_path(u32 fd) {
  auto f = get(fd);
  return f ? f->path : std::string();
}

}  // namespace rt::vfs
