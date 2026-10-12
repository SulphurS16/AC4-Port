// HLE: sys_fs (cellFs*) and LV2 file syscalls 801-834, both on top of rt::vfs.
#include <deque>

#include "runtime/hle.h"
#include "runtime/ppu_thread.h"
#include "runtime/vfs.h"

namespace rt {

#pragma pack(push, 4)
struct CellFsStat {
  bes32 mode, uid, gid;
  bes64 atime, mtime, ctime;
  be64 size, blksize;
};
#pragma pack(pop)
static_assert(sizeof(CellFsStat) == 52);
struct CellFsDirent {
  u8 d_type, d_namlen;
  char d_name[256];
};

namespace {

// ---- asynchronous I/O (cellFsAio*): one worker thread performs requests in order and calls the
// guest completion callback func(aio, error, id, transferred) ----
struct CellFsAio {
  be32 fd, pad;
  be64 offset;
  be32 buf, pad2;
  be64 size, user_data;
};
static_assert(sizeof(CellFsAio) == 0x28);
struct AioReq {
  bool write;
  s32 id;
  u32 aio, func;
};
std::mutex g_aio_mu;
std::condition_variable g_aio_cv;
std::deque<AioReq> g_aio_q;
bool g_aio_started = false;
s32 g_aio_next = 0;

void aio_worker(ppu::Context& ctx) {
  for (;;) {
    AioReq r;
    {
      std::unique_lock lk(g_aio_mu);
      g_aio_cv.wait(lk, [] { return !g_aio_q.empty(); });
      r = g_aio_q.front();
      g_aio_q.pop_front();
    }
    const CellFsAio* a = gp<CellFsAio>(r.aio);
    u64 done = 0;
    u32 fd = a->fd, buf = a->buf;
    u64 off = a->offset, size = a->size;
    s32 err = r.write ? vfs::pwrite(fd, off, ppu::mem() + buf, size, &done) : vfs::pread(fd, off, ppu::mem() + buf, size, &done);
    LOG_D("fs", "aio %s #%d fd %u off 0x%llx size 0x%llx -> 0x%x (0x%llx bytes)", r.write ? "write" : "read", r.id, fd,
          (unsigned long long)off, (unsigned long long)size, u32(err), (unsigned long long)done);
    hle::call_guest(ctx, r.func, {r.aio, u64(s64(err)), u64(s64(r.id)), done});
  }
}

s32 cellFsAioInit(u32 mount_point) {
  std::string mp = read_cstr(mount_point);
  std::lock_guard lk(g_aio_mu);
  if (!g_aio_started) {
    g_aio_started = true;
    thread_create_host("_cellFsAio", 1000, 0x10000, aio_worker);
  }
  LOG_I("fs", "cellFsAioInit('%s')", mp.c_str());
  return CELL_OK;
}
s32 cellFsAioFinish(u32 mount_point) { return CELL_OK; }
s32 aio_submit(bool write, u32 aio, Ptr<bes32> id, u32 func) {
  if (!aio || !id || !func) return CELL_EFAULT;
  std::lock_guard lk(g_aio_mu);
  if (!g_aio_started) return CELL_ENXIO;
  s32 x = ++g_aio_next;
  *id = x;
  g_aio_q.push_back({write, x, aio, func});
  g_aio_cv.notify_one();
  return CELL_OK;
}
s32 cellFsAioRead(u32 aio, Ptr<bes32> id, u32 func) { return aio_submit(false, aio, id, func); }
s32 cellFsAioWrite(u32 aio, Ptr<bes32> id, u32 func) { return aio_submit(true, aio, id, func); }

void put_stat(Ptr<CellFsStat> sb, const vfs::Stat& st, bool disc) {
  s32 mode = st.is_dir ? (0040000 | 0777) : (0100000 | 0666);
  if (st.readonly || disc) mode &= ~0222;
  sb->mode = mode;
  sb->uid = 0;
  sb->gid = 0;
  sb->atime = st.atime;
  sb->mtime = st.mtime;
  sb->ctime = st.ctime;
  sb->size = st.size;
  sb->blksize = disc ? 2048 : 4096;
}

bool is_disc(const std::string& p) {
  vfs::Resolved r;
  return vfs::resolve(p, r) && !r.writable;
}

s32 cellFsOpen(u32 path, s32 flags, Ptr<be32> fd, u32 arg, u64 size) {
  if (!path || !fd) return CELL_EFAULT;
  std::string p = read_cstr(path);
  u32 f = 0;
  s32 r = vfs::open(p, flags, &f);
  if (r == CELL_OK) *fd = f;
  LOG_I("fs", "cellFsOpen('%s', 0x%x) -> 0x%x fd %u", p.c_str(), flags, u32(r), f);
  return r;
}
s32 cellFsRead(u32 fd, u32 buf, u64 n, Ptr<be64> nread) {
  u64 done = 0;
  s32 r = vfs::read(fd, ppu::mem() + buf, n, &done);
  if (nread) *nread = done;
  return r;
}
s32 cellFsWrite(u32 fd, u32 buf, u64 n, Ptr<be64> nwrite) {
  u64 done = 0;
  s32 r = vfs::write(fd, ppu::mem() + buf, n, &done);
  if (nwrite) *nwrite = done;
  return r;
}
s32 cellFsClose(u32 fd) { return vfs::close(fd); }
s32 cellFsLseek(u32 fd, s64 off, u32 whence, Ptr<be64> pos) {
  u64 p = 0;
  s32 r = vfs::lseek(fd, off, s32(whence), &p);
  if (r == CELL_OK && pos) *pos = p;
  return r;
}
s32 cellFsFstat(u32 fd, Ptr<CellFsStat> sb) {
  vfs::Stat st;
  s32 r = vfs::fstat(fd, &st);
  if (r == CELL_OK) put_stat(sb, st, is_disc(vfs::fd_path(fd)));
  return r;
}
s32 cellFsStat(u32 path, Ptr<CellFsStat> sb) {
  std::string p = read_cstr(path);
  vfs::Stat st;
  s32 r = vfs::stat(p, &st);
  if (r == CELL_OK) put_stat(sb, st, is_disc(p));
  LOG_D("fs", "cellFsStat('%s') -> 0x%x", p.c_str(), u32(r));
  return r;
}
s32 cellFsOpendir(u32 path, Ptr<be32> fd) {
  std::string p = read_cstr(path);
  u32 f = 0;
  s32 r = vfs::opendir(p, &f);
  if (r == CELL_OK) *fd = f;
  LOG_D("fs", "cellFsOpendir('%s') -> 0x%x", p.c_str(), u32(r));
  return r;
}
s32 cellFsReaddir(u32 fd, Ptr<CellFsDirent> d, Ptr<be64> nread) {
  bool got = false;
  u8 type = 0;
  std::string name;
  s32 r = vfs::readdir(fd, &got, &type, &name);
  if (r != CELL_OK) return r;
  if (!got) {
    *nread = 0;
    return CELL_OK;
  }
  d->d_type = type;
  d->d_namlen = u8(std::min<size_t>(name.size(), 255));
  write_cstr(d.addr + 2, name, 256);
  *nread = sizeof(CellFsDirent);
  return CELL_OK;
}
s32 cellFsClosedir(u32 fd) { return vfs::closedir(fd); }
s32 cellFsMkdir(u32 path, s32 mode) { return vfs::mkdir(read_cstr(path)); }
s32 cellFsRmdir(u32 path) { return vfs::rmdir(read_cstr(path)); }
s32 cellFsUnlink(u32 path) { return vfs::unlink(read_cstr(path)); }
s32 cellFsRename(u32 from, u32 to) { return vfs::rename(read_cstr(from), read_cstr(to)); }
s32 cellFsFtruncate(u32 fd, u64 size) { return vfs::ftruncate(fd, size); }
s32 cellFsFGetBlockSize(u32 fd, Ptr<be64> sector, Ptr<be64> block) {
  vfs::Stat st;
  s32 r = vfs::fstat(fd, &st);
  if (r != CELL_OK) return r;
  u64 bs = is_disc(vfs::fd_path(fd)) ? 2048 : 4096;
  if (sector) *sector = bs;
  if (block) *block = bs;
  return CELL_OK;
}

// ---- LV2 syscalls ----
s32 sys_fs_open(u32 path, s32 flags, Ptr<be32> fd, s32 mode, u32 arg, u64 size) { return cellFsOpen(path, flags, fd, arg, size); }
s32 sys_fs_read(u32 fd, u32 buf, u64 n, Ptr<be64> nread) { return cellFsRead(fd, buf, n, nread); }
s32 sys_fs_write(u32 fd, u32 buf, u64 n, Ptr<be64> nwrite) { return cellFsWrite(fd, buf, n, nwrite); }
s32 sys_fs_close(u32 fd) { return cellFsClose(fd); }
s32 sys_fs_opendir(u32 path, Ptr<be32> fd) { return cellFsOpendir(path, fd); }
s32 sys_fs_readdir(u32 fd, Ptr<CellFsDirent> d, Ptr<be64> nread) { return cellFsReaddir(fd, d, nread); }
s32 sys_fs_closedir(u32 fd) { return cellFsClosedir(fd); }
s32 sys_fs_stat(u32 path, Ptr<CellFsStat> sb) { return cellFsStat(path, sb); }
s32 sys_fs_fstat(u32 fd, Ptr<CellFsStat> sb) { return cellFsFstat(fd, sb); }
s32 sys_fs_mkdir(u32 path, s32 mode) { return cellFsMkdir(path, mode); }
s32 sys_fs_rename(u32 from, u32 to) { return cellFsRename(from, to); }
s32 sys_fs_rmdir(u32 path) { return cellFsRmdir(path); }
s32 sys_fs_unlink(u32 path) { return cellFsUnlink(path); }
s32 sys_fs_lseek(u32 fd, s64 off, s32 whence, Ptr<be64> pos) { return cellFsLseek(fd, off, u32(whence), pos); }
s32 sys_fs_fsync(u32 fd) { return CELL_OK; }
s32 sys_fs_truncate(u32 path, u64 size) { return vfs::truncate(read_cstr(path), size); }
s32 sys_fs_ftruncate(u32 fd, u64 size) { return vfs::ftruncate(fd, size); }
s32 sys_fs_chmod(u32 path, s32 mode) {
  vfs::Stat st;
  return vfs::stat(read_cstr(path), &st);
}
s32 sys_fs_utime(u32 path, u32 timep) { return vfs::utime(read_cstr(path), 0, 0); }
s32 sys_fs_link(u32 from, u32 to) {
  LOG_W("fs", "sys_fs_link('%s','%s') not supported", read_cstr(from).c_str(), read_cstr(to).c_str());
  return CELL_ENOTSUP;
}

hle::SyscallTable s_fs({
    SYSCALL(801, sys_fs_open),
    SYSCALL(802, sys_fs_read),
    SYSCALL(803, sys_fs_write),
    SYSCALL(804, sys_fs_close),
    SYSCALL(805, sys_fs_opendir),
    SYSCALL(806, sys_fs_readdir),
    SYSCALL(807, sys_fs_closedir),
    SYSCALL(808, sys_fs_stat),
    SYSCALL(809, sys_fs_fstat),
    SYSCALL(810, sys_fs_link),
    SYSCALL(811, sys_fs_mkdir),
    SYSCALL(812, sys_fs_rename),
    SYSCALL(813, sys_fs_rmdir),
    SYSCALL(814, sys_fs_unlink),
    SYSCALL(815, sys_fs_utime),
    SYSCALL(818, sys_fs_lseek),
    SYSCALL(820, sys_fs_fsync),
    SYSCALL(831, sys_fs_truncate),
    SYSCALL(832, sys_fs_ftruncate),
    SYSCALL(834, sys_fs_chmod),
});

}  // namespace

static hle::Module s_module("sys_fs", {
    HLE_FN(cellFsAioFinish),
    HLE_FN(cellFsAioInit),
    HLE_FN(cellFsAioRead),
    HLE_FN(cellFsAioWrite),
    HLE_FN(cellFsClose),
    HLE_FN(cellFsClosedir),
    HLE_FN(cellFsFGetBlockSize),
    HLE_FN(cellFsFstat),
    HLE_FN(cellFsFtruncate),
    HLE_FN(cellFsLseek),
    HLE_FN(cellFsMkdir),
    HLE_FN(cellFsOpen),
    HLE_FN(cellFsOpendir),
    HLE_FN(cellFsRead),
    HLE_FN(cellFsReaddir),
    HLE_FN(cellFsRename),
    HLE_FN(cellFsRmdir),
    HLE_FN(cellFsStat),
    HLE_FN(cellFsUnlink),
    HLE_FN(cellFsWrite),
});

}  // namespace rt
