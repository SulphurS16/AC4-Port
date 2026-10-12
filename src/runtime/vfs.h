// Guest file system: maps LV2 mount points to host directories.
//   /dev_bdvd  -> disc root (read-only)        /app_home -> disc PS3_GAME/USRDIR (read-only)
//   /dev_hdd0  -> local/hdd0 (writable)         /dev_hdd1 -> local/hdd1 (writable)
//   /dev_flash -> extracted firmware dev_flash (read-only, from the user's PS3UPDAT.PUP)
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::vfs {

void init(const std::string& disc_root, const std::string& hdd0, const std::string& hdd1, const std::string& flash);

struct Resolved {
  std::filesystem::path host;
  bool writable = false;
  std::string mount;
};
// Returns false for paths outside every mount point (or escaping one with "..").
bool resolve(const std::string& guest_path, Resolved& out);

// File descriptors shared by cellFs* and sys_fs_* syscalls.
struct Stat {
  bool is_dir = false;
  u64 size = 0;
  s64 atime = 0, mtime = 0, ctime = 0;
  bool readonly = false;
};
s32 open(const std::string& path, s32 flags, u32* fd);
s32 close(u32 fd);
s32 read(u32 fd, void* buf, u64 n, u64* done);
s32 write(u32 fd, const void* buf, u64 n, u64* done);
s32 pread(u32 fd, u64 offset, void* buf, u64 n, u64* done);   // does not move the file position
s32 pwrite(u32 fd, u64 offset, const void* buf, u64 n, u64* done);
s32 lseek(u32 fd, s64 off, s32 whence, u64* pos);
s32 fstat(u32 fd, Stat* st);
s32 stat(const std::string& path, Stat* st);
s32 ftruncate(u32 fd, u64 size);
s32 truncate(const std::string& path, u64 size);
s32 opendir(const std::string& path, u32* fd);
s32 readdir(u32 fd, bool* got, u8* type, std::string* name);
s32 closedir(u32 fd);
s32 mkdir(const std::string& path);
s32 rmdir(const std::string& path);
s32 unlink(const std::string& path);
s32 rename(const std::string& from, const std::string& to);
s32 utime(const std::string& path, s64 atime, s64 mtime);
std::string fd_path(u32 fd);

}  // namespace rt::vfs
