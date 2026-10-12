// LV2 process syscalls.
#include <fstream>

#include "runtime/lv2/lv2.h"

namespace rt::lv2 {

u32 g_sdk_version = 0x220002;
u64 g_auth_id = 0;
u32 g_ctrl_flags1 = 0;
constexpr u32 kPid = 0x01000500;

bool read_self_identity(const std::string& self_path) {
  std::ifstream f(self_path, std::ios::binary);
  std::vector<u8> d(0x1000);
  if (!f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size())) && f.gcount() < 0x80) return false;
  d.resize(size_t(f.gcount() > 0 ? f.gcount() : 0));
  if (d.size() < 0x80 || ac4::rd_be32(&d[0]) != 0x53434500) return false;  // "SCE\0"
  u64 appinfo = ac4::rd_be64(&d[0x28]), ctrl = ac4::rd_be64(&d[0x58]), ctrl_size = ac4::rd_be64(&d[0x60]);
  if (appinfo + 8 > d.size()) return false;
  g_auth_id = ac4::rd_be64(&d[appinfo]);
  // Control info entries {u32 type, u32 size, u64 next, ...}; type 1 holds the control flags.
  for (u64 a = ctrl; a + 0x14 <= d.size() && a < ctrl + ctrl_size;) {
    u32 type = ac4::rd_be32(&d[a]), size = ac4::rd_be32(&d[a + 4]);
    if (type == 1) g_ctrl_flags1 = ac4::rd_be32(&d[a + 0x10]);
    if (!size) break;
    a += size;
  }
  return true;
}

namespace {

constexpr s32 SYS_SS_ERROR_UNKNOWN_PACKAGE = s32(0x8001051d);

u32 sys_process_getpid() { return kPid; }
s32 sys_process_get_sdk_version(u32 pid, Ptr<bes32> version) {
  if (!version) return CELL_EFAULT;
  *version = s32(g_sdk_version);
  return CELL_OK;
}
s32 sys_process_getppid() { return 0x01000300; }

// Debug/root permissions come from the SELF control flags; a retail game has neither.
bool debug_or_root() { return (g_ctrl_flags1 & (0xeu << 28)) != 0; }

s32 sys_ss_access_control_engine(u64 pkg_id, u64 a2, u64 a3) {
  switch (pkg_id) {
    case 1:
      if (!debug_or_root()) return CELL_ENOSYS;
      if (!a2) return CELL_ESRCH;
      ppu::w64(u32(a3), g_auth_id);
      return CELL_OK;
    case 2:  // the caller's program authority ID
      ppu::w64(u32(a2), g_auth_id);
      return CELL_OK;
    case 3:
      return debug_or_root() ? CELL_OK : CELL_ENOSYS;
    default:
      return SYS_SS_ERROR_UNKNOWN_PACKAGE;
  }
}
s32 sys_process_get_number_of_object(u32 type, Ptr<be32> nump) {
  if (nump) *nump = 0;
  return CELL_OK;
}

hle::SyscallTable s_proc({
    SYSCALL(1, sys_process_getpid),
    SYSCALL(18, sys_process_getppid),
    SYSCALL(12, sys_process_get_number_of_object),
    SYSCALL(25, sys_process_get_sdk_version),
    SYSCALL(871, sys_ss_access_control_engine),
});

}  // namespace
}  // namespace rt::lv2
