// LV2 event flags (64-bit pattern, AND/OR waits with optional clear).
#include "runtime/lv2/lv2.h"

namespace rt::lv2 {
namespace {

struct Attr {
  be32 protocol, pshared;
  be64 ipc_key;
  bes32 flags, type;
  char name[8];
};

struct EventFlag {
  std::mutex m;
  std::condition_variable cv;
  u64 pattern = 0;
  s32 type = 0x10000;
  u32 waiters = 0;
  u64 cancel_gen = 0;
  std::string name;
};
IdMap<EventFlag> g_flags(0x98000000);

bool satisfied(u64 pattern, u64 bits, u32 mode) {
  return (mode & 0xf) == 0x01 ? (pattern & bits) == bits : (pattern & bits) != 0;
}
void apply_clear(EventFlag& f, u64 bits, u32 mode) {
  if (mode & 0x10) f.pattern &= ~bits;
  if (mode & 0x20) f.pattern = 0;
}

s32 sys_event_flag_create(Ptr<be32> id, Ptr<Attr> attr, u64 init) {
  if (!id || !attr) return CELL_EFAULT;
  s32 type = attr->type;
  if (type != 0x10000 && type != 0x20000) return CELL_EINVAL;
  auto f = std::make_shared<EventFlag>();
  f->pattern = init;
  f->type = type;
  f->name = name8(attr->name);
  *id = g_flags.add(f);
  return CELL_OK;
}
s32 sys_event_flag_destroy(u32 id) {
  auto f = g_flags.get(id);
  if (!f) return CELL_ESRCH;
  {
    std::lock_guard lk(f->m);
    if (f->waiters) return CELL_EBUSY;
  }
  g_flags.remove(id);
  return CELL_OK;
}
s32 sys_event_flag_wait(u32 id, u64 bits, u32 mode, Ptr<be64> result, u64 timeout) {
  if (result) *result = 0;
  if (!bits || ((mode & 0xf) != 0x01 && (mode & 0xf) != 0x02) || (mode & ~0x3fu)) return CELL_EINVAL;
  auto f = g_flags.get(id);
  if (!f) return CELL_ESRCH;
  std::unique_lock lk(f->m);
  if (f->type == 0x10000 && f->waiters) return CELL_EPERM;
  u64 gen = f->cancel_gen;
  ++f->waiters;
  bool ok = wait_us(f->cv, lk, timeout, [&] { return satisfied(f->pattern, bits, mode) || f->cancel_gen != gen; });
  --f->waiters;
  if (f->cancel_gen != gen) return CELL_ECANCELED;
  if (!ok) return CELL_ETIMEDOUT;
  if (result) *result = f->pattern;
  apply_clear(*f, bits, mode);
  return CELL_OK;
}
s32 sys_event_flag_trywait(u32 id, u64 bits, u32 mode, Ptr<be64> result) {
  if (result) *result = 0;
  if (!bits || ((mode & 0xf) != 0x01 && (mode & 0xf) != 0x02)) return CELL_EINVAL;
  auto f = g_flags.get(id);
  if (!f) return CELL_ESRCH;
  std::lock_guard lk(f->m);
  if (!satisfied(f->pattern, bits, mode)) return CELL_EBUSY;
  if (result) *result = f->pattern;
  apply_clear(*f, bits, mode);
  return CELL_OK;
}
s32 sys_event_flag_set(u32 id, u64 bits) {
  auto f = g_flags.get(id);
  if (!f) return CELL_ESRCH;
  {
    std::lock_guard lk(f->m);
    f->pattern |= bits;
  }
  f->cv.notify_all();
  return CELL_OK;
}
s32 sys_event_flag_clear(u32 id, u64 bits) {
  auto f = g_flags.get(id);
  if (!f) return CELL_ESRCH;
  std::lock_guard lk(f->m);
  f->pattern &= bits;
  return CELL_OK;
}
s32 sys_event_flag_cancel(u32 id, Ptr<be32> num) {
  auto f = g_flags.get(id);
  if (!f) return CELL_ESRCH;
  u32 n;
  {
    std::lock_guard lk(f->m);
    n = f->waiters;
    ++f->cancel_gen;
  }
  f->cv.notify_all();
  if (num) *num = n;
  return CELL_OK;
}
s32 sys_event_flag_get(u32 id, Ptr<be64> flags) {
  auto f = g_flags.get(id);
  if (!f) {
    if (flags) *flags = 0;
    return CELL_ESRCH;
  }
  if (!flags) return CELL_EFAULT;
  std::lock_guard lk(f->m);
  *flags = f->pattern;
  return CELL_OK;
}

hle::SyscallTable s_ef({
    SYSCALL(82, sys_event_flag_create),
    SYSCALL(83, sys_event_flag_destroy),
    SYSCALL(85, sys_event_flag_wait),
    SYSCALL(86, sys_event_flag_trywait),
    SYSCALL(87, sys_event_flag_set),
    SYSCALL(118, sys_event_flag_clear),
    SYSCALL(132, sys_event_flag_cancel),
    SYSCALL(139, sys_event_flag_get),
});

}  // namespace

// Used by the SPU interrupt-mailbox protocol (event flag set from SPU).
s32 event_flag_set_from_spu(u32 id, u64 bits) { return sys_event_flag_set(id, bits); }

}  // namespace rt::lv2
