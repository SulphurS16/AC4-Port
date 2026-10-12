// HLE: sysPrxForUser (liblv2 user-space helpers).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <set>

#include "runtime/config.h"
#include "runtime/guest_printf.h"
#include "runtime/hle.h"
#include "runtime/spu/spu_image.h"
#include "runtime/lv2/lv2.h"
#include "runtime/lv2/memory.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"
#include "runtime/ppu_thread.h"

namespace rt {
namespace lv2 {
void sleep_us(u64 us);
}
namespace {

using lv2::self_id;

// ---- lightweight mutex ----
constexpr u32 LWMUTEX_FREE = 0xffffffffu;
struct LwMutexGuest {
  be32 owner, waiter, attribute, recursive_count, sleep_queue, pad;
};
struct LwMutexAttr {
  be32 protocol, recursive;
  char name[8];
};
struct LwMutex {
  std::mutex m;
  std::condition_variable cv;
  lv2::FairOwner o;  // hand-off ownership (protocol from the attribute)
  bool recursive = false;
};
std::mutex g_lw_mu;
std::map<u32, std::shared_ptr<LwMutex>> g_lw;
u32 g_lw_next = 0x95000000;

std::shared_ptr<LwMutex> lw_get(u32 addr, bool create_if_missing) {
  std::lock_guard lk(g_lw_mu);
  auto it = g_lw.find(addr);
  if (it != g_lw.end()) return it->second;
  if (!create_if_missing) return nullptr;
  auto g = gp<LwMutexGuest>(addr);
  auto lw = std::make_shared<LwMutex>();
  lw->recursive = (g->attribute & 0xf0) == 0x10;
  lw->o.protocol = g->attribute & 0xf;
  g_lw[addr] = lw;
  LOG_D("lwmutex", "lwmutex at 0x%08x used without sys_lwmutex_create (attr 0x%x)", addr, u32(g->attribute));
  return lw;
}

s32 sys_lwmutex_create(Ptr<LwMutexGuest> lw, Ptr<LwMutexAttr> attr) {
  if (!lw || !attr) return CELL_EFAULT;
  u32 rec = attr->recursive;
  if (rec != 0x10 && rec != 0x20) return CELL_EINVAL;
  auto o = std::make_shared<LwMutex>();
  o->recursive = rec == 0x10;
  o->o.protocol = attr->protocol & 0xf;
  {
    std::lock_guard lk(g_lw_mu);
    g_lw[lw.addr] = o;
    lw->sleep_queue = g_lw_next++;
  }
  lw->owner = LWMUTEX_FREE;
  lw->waiter = 0;
  lw->attribute = u32(attr->protocol) | rec;
  lw->recursive_count = 0;
  lw->pad = 0;
  return CELL_OK;
}
s32 sys_lwmutex_destroy(Ptr<LwMutexGuest> lw) {
  if (log_level() <= Level::Debug)  // which code destroys mutexes: object lifetimes
    if (PpuThread* t = cur_thread())
      LOG_D("lwmutex", "destroy 0x%08x in '%s', backtrace%s", lw.addr, t->name.c_str(), guest_backtrace(t->ctx).c_str());
  auto o = lw_get(lw.addr, false);
  if (!o) return CELL_ESRCH;
  {
    std::lock_guard lk(o->m);
    if (o->o.owner || !o->o.queue.empty()) return CELL_EBUSY;  // LV2: owned, or threads still waiting
  }
  std::lock_guard lk(g_lw_mu);
  g_lw.erase(lw.addr);
  lw->owner = 0xfffffffe;  // dead
  return CELL_OK;
}
s32 lw_lock(Ptr<LwMutexGuest> lw, u64 timeout, bool try_only) {
  if (u32(lw->owner) == 0xfffffffe) return CELL_EINVAL;  // destroyed (liblv2 reports EINVAL)
  auto o = lw_get(lw.addr, true);
  u32 me = self_id();
  std::unique_lock lk(o->m);
  if (o->o.owner == me) {
    if (!o->recursive) return try_only ? CELL_EBUSY : CELL_EDEADLK;
    ++o->o.count;
    lw->recursive_count = o->o.count;
    return CELL_OK;
  }
  if (try_only) {
    if (o->o.owner || (!o->o.queue.empty() && o->o.protocol != 4)) return CELL_EBUSY;
    o->o.owner = me;
  } else {
    lw->waiter = u32(lw->waiter) + 1;
    bool ok = lv2::fair_acquire(o->o, o->cv, lk, me, timeout);
    lw->waiter = u32(lw->waiter) - 1;
    if (!ok) return CELL_ETIMEDOUT;
  }
  o->o.count = 1;
  lw->owner = me;
  lw->recursive_count = 1;
  return CELL_OK;
}
s32 sys_lwmutex_lock(Ptr<LwMutexGuest> lw, u64 timeout) { return lw_lock(lw, timeout, false); }
s32 sys_lwmutex_trylock(Ptr<LwMutexGuest> lw) { return lw_lock(lw, 0, true); }
s32 sys_lwmutex_unlock(Ptr<LwMutexGuest> lw) {
  auto o = lw_get(lw.addr, false);
  if (!o) return CELL_ESRCH;
  std::unique_lock lk(o->m);
  if (o->o.owner != self_id()) return CELL_EPERM;
  if (--o->o.count == 0) {
    bool wake = lv2::fair_release(o->o);
    lw->owner = o->o.owner ? o->o.owner : LWMUTEX_FREE;
    lw->recursive_count = o->o.owner ? 1 : 0;
    if (wake) o->cv.notify_all();
  } else {
    lw->recursive_count = o->o.count;
  }
  return CELL_OK;
}

}  // namespace

// Logs every lightweight mutex that is currently held (diagnostics).
void lwmutex_dump() {
  std::lock_guard lk(g_lw_mu);
  for (auto& [addr, o] : g_lw) {
    std::lock_guard ol(o->m);
    if (!o->o.owner) continue;
    auto g = gp<LwMutexGuest>(addr);
    LOG_I("dump", "lwmutex 0x%08x held by 0x%x (count %u, protocol %u), %u waiter(s)", addr, o->o.owner, o->o.count,
          o->o.protocol, u32(g->waiter));
  }
}

namespace {

// ---- threads ----
s32 sys_ppu_thread_create(Ptr<be64> tid, u32 entry, u64 arg, s32 prio, u32 stacksize, u64 flags, u32 name) {
  if (prio < 0 || prio > 3071) return CELL_EINVAL;
  if (flags & 2) {
    LOG_E("thread", "interrupt PPU threads are not supported (flags 0x%llx)", (unsigned long long)flags);
    return CELL_EPERM;
  }
  std::string n = read_cstr(name, 64);
  s32 err = CELL_OK;
  u32 id = thread_create(entry, arg, prio, stacksize, (flags & 1) != 0, n, &err);
  if (!id) return err;
  if (tid) *tid = id;
  return thread_start(id);
}
void sys_ppu_thread_exit(u64 val) { thread_exit(val); }
s32 sys_ppu_thread_get_id(Ptr<be64> out) {
  *out = self_id();
  return CELL_OK;
}

// ---- process ----
void sys_process_exit(s32 status) { process_exit(status); }
void _sys_process_atexitspawn() { LOG_D("process", "_sys_process_atexitspawn (ignored: no exitspawn support)"); }
void _sys_process_at_Exitspawn() { LOG_D("process", "_sys_process_at_Exitspawn (ignored: no exitspawn support)"); }
void sys_prx_exitspawn_with_level() {
  LOG_E("process", "sys_prx_exitspawn_with_level: the game asked to spawn another executable; exiting");
  process_exit(0);
}

// ---- TLS / time ----
void sys_initialize_tls(ppu::Ctx& ctx, u64 main_thread_id, u32 addr, u32 size, u32 mem_size) {
  tls_initialize(ctx, addr, size, mem_size);
}
u64 sys_time_get_system_time() { return lv2::system_time_us(); }

// ---- heap (liblv2 _sys_heap_*): backed by page allocations in the main area ----
std::mutex g_heap_mu;
u32 g_heap_next = 1;
std::map<u32, u32> g_heap_blocks;  // addr -> heap
u32 _sys_heap_create_heap(u32 name, u32 a2, u32 a3, u32 a4) {
  std::lock_guard lk(g_heap_mu);
  u32 id = g_heap_next++;
  LOG_D("heap", "_sys_heap_create_heap('%s', 0x%x, 0x%x, 0x%x) -> %u", read_cstr(name).c_str(), a2, a3, a4, id);
  return id;
}
s32 _sys_heap_delete_heap(u32 heap) { return CELL_OK; }
u32 _sys_heap_malloc(u32 heap, u32 size) {
  u32 a = vm::alloc(vm::Area::Main, size ? size : 1, 0x10);
  LOG_D("heap", "_sys_heap_malloc(%u, 0x%x) -> 0x%08x", heap, size, a);
  return a;
}
u32 _sys_heap_memalign(u32 heap, u32 align, u32 size) {
  u32 a = vm::alloc(vm::Area::Main, size ? size : 1, std::max<u32>(align, 0x10));
  LOG_D("heap", "_sys_heap_memalign(%u, 0x%x, 0x%x) -> 0x%08x", heap, align, size, a);
  return a;
}
s32 _sys_heap_free(u32 heap, u32 addr) {
  vm::free(vm::Area::Main, addr);
  return CELL_OK;
}

// ---- spinlock ----
s32 sys_spinlock_initialize(Ptr<be32> lock) {
  *lock = 0;
  return CELL_OK;
}
s32 sys_spinlock_lock(Ptr<be32> lock) {
  auto* p = reinterpret_cast<std::atomic<u32>*>(lock.get());
  u32 me = __builtin_bswap32(0xabadcafe);
  for (u32 spins = 0;; ++spins) {
    u32 expected = 0;
    if (p->compare_exchange_weak(expected, me)) return CELL_OK;
    if (spins > 64) SwitchToThread();
    else _mm_pause();
  }
}
s32 sys_spinlock_trylock(Ptr<be32> lock) {
  auto* p = reinterpret_cast<std::atomic<u32>*>(lock.get());
  u32 expected = 0;
  return p->compare_exchange_strong(expected, __builtin_bswap32(0xabadcafe)) ? CELL_OK : CELL_EBUSY;
}
s32 sys_spinlock_unlock(Ptr<be32> lock) {
  reinterpret_cast<std::atomic<u32>*>(lock.get())->store(0);
  return CELL_OK;
}

// ---- mmapper memory objects ----
s32 sys_mmapper_allocate_memory(u32 size, u64 flags, Ptr<be32> mem_id) {
  u32 id = 0;
  s32 r = lv2::mmapper_allocate_memory(size, flags, &id);
  if (r == CELL_OK) *mem_id = id;
  return r;
}
s32 sys_mmapper_free_memory(u32 mem_id) { return lv2::mmapper_free_memory(mem_id); }
s32 sys_mmapper_unmap_memory(u32 addr, Ptr<be32> mem_id) {
  u32 id = 0;
  s32 r = lv2::mmapper_unmap_memory(addr, &id);
  if (r == CELL_OK) *mem_id = id;
  return r;
}

// ---- libc-style helpers (used by firmware modules such as libsre) ----
u32 _sys_memset(u32 dst, s32 v, u32 n) {
  std::memset(ppu::mem() + dst, v, n);
  return dst;
}
u32 _sys_memcpy(u32 dst, u32 src, u32 n) {
  std::memmove(ppu::mem() + dst, ppu::mem() + src, n);
  return dst;
}
s32 _sys_memcmp(u32 a, u32 b, u32 n) { return std::memcmp(ppu::mem() + a, ppu::mem() + b, n); }
u32 _sys_strlen(u32 s) { return u32(read_cstr(s, 1u << 20).size()); }
u32 _sys_strcpy(u32 dst, u32 src) {
  std::string s = read_cstr(src, 1u << 20);
  std::memcpy(ppu::mem() + dst, s.c_str(), s.size() + 1);
  return dst;
}
u32 _sys_strncpy(u32 dst, u32 src, u32 n) {
  std::strncpy(gp<char>(dst), gp<char>(src), n);
  return dst;
}
s32 _sys_strcmp(u32 a, u32 b) { return std::strcmp(gp<char>(a), gp<char>(b)); }
s32 _sys_strncmp(u32 a, u32 b, u32 n) { return std::strncmp(gp<char>(a), gp<char>(b), n); }
u32 _sys_strchr(u32 s, s32 c) {
  const char* p = std::strchr(gp<char>(s), c);
  return p ? s + u32(p - gp<char>(s)) : 0;
}
u32 _sys_strcat(u32 dst, u32 src) {
  std::string a = read_cstr(dst, 1u << 20), b = read_cstr(src, 1u << 20);
  std::memcpy(ppu::mem() + dst + a.size(), b.c_str(), b.size() + 1);
  return dst;
}
s32 put_formatted(u32 buf, u32 size, const std::string& s) {
  if (size) write_cstr(buf, s, size);
  return s32(s.size());
}
void _sys_snprintf(ppu::Ctx& ctx) {
  RegArgs args(ctx, 6);
  std::string s = guest_format(read_cstr(u32(ctx.r[5])), args);
  ctx.r[3] = u64(s64(put_formatted(u32(ctx.r[3]), u32(ctx.r[4]), s)));
}
s32 _sys_vsnprintf(u32 buf, u32 size, u32 fmt, u32 va) {
  VaListArgs args(va);
  return put_formatted(buf, size, guest_format(read_cstr(fmt), args));
}
void _sys_printf(ppu::Ctx& ctx) {
  RegArgs args(ctx, 4);
  std::string s = guest_format(read_cstr(u32(ctx.r[3])), args);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  LOG_I("printf", "%s", s.c_str());
  ctx.r[3] = s.size();
}
s32 _sys_vprintf(u32 fmt, u32 va) {
  VaListArgs args(va);
  std::string s = guest_format(read_cstr(fmt), args);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  LOG_I("printf", "%s", s.c_str());
  return s32(s.size());
}

// ---- SPU images ----
s32 sys_spu_image_import(u32 img, u32 src, u32 type) { return spu::image_import(img, src, type); }
s32 sys_spu_image_close(u32 img) { return spu::image_close(img); }

// ---- process / module queries ----
s32 sys_process_get_paramsfo(u32 buf) {
  std::memset(ppu::mem() + buf, 0, 0x40);
  const std::string& id = cfg().title_id;
  std::memcpy(ppu::mem() + buf + 1, id.data(), std::min<size_t>(id.size(), 9));
  return CELL_OK;
}
s32 sys_prx_get_module_id_by_name(u32 name, u64 flags, u32 opt) {
  LOG_D("prx", "sys_prx_get_module_id_by_name('%s') -> not loaded", read_cstr(name).c_str());
  return s32(0x8001112e);  // CELL_PRX_ERROR_UNKNOWN_MODULE
}
// No spu_printf server is installed through liblv2 (the game uses its own via SPURS queues).
s32 _sys_spu_printf_attach_group(u32 group) { return CELL_ESTAT; }
s32 _sys_spu_printf_detach_group(u32 group) { return CELL_ESTAT; }

}  // namespace

static hle::Module s_profmodule("cellLibprof", {
    ::rt::hle::Func{"cellUserTraceRegister", [](ppu::Ctx& c) { c.r[3] = 0; }, 0},
    ::rt::hle::Func{"cellUserTraceUnregister", [](ppu::Ctx& c) { c.r[3] = 0; }, 0},
});

static hle::Module s_module("sysPrxForUser", {
    HLE_FN(_sys_memcmp),
    HLE_FN(_sys_memcpy),
    HLE_FN(_sys_memset),
    HLE_RAW(_sys_printf),
    HLE_RAW(_sys_snprintf),
    HLE_FN(_sys_spu_printf_attach_group),
    HLE_FN(_sys_spu_printf_detach_group),
    HLE_FN(_sys_strcat),
    HLE_FN(_sys_strchr),
    HLE_FN(_sys_strcmp),
    HLE_FN(_sys_strcpy),
    HLE_FN(_sys_strlen),
    HLE_FN(_sys_strncmp),
    HLE_FN(_sys_strncpy),
    HLE_FN(_sys_vprintf),
    HLE_FN(_sys_vsnprintf),
    HLE_FN(sys_process_get_paramsfo),
    HLE_FN(sys_prx_get_module_id_by_name),
    HLE_FN(_sys_heap_create_heap),
    HLE_FN(_sys_heap_delete_heap),
    HLE_FN(_sys_heap_free),
    HLE_FN(_sys_heap_malloc),
    HLE_FN(_sys_heap_memalign),
    HLE_FN(_sys_process_at_Exitspawn),
    HLE_FN(_sys_process_atexitspawn),
    HLE_FN(sys_initialize_tls),
    HLE_FN(sys_lwmutex_create),
    HLE_FN(sys_lwmutex_destroy),
    HLE_FN(sys_lwmutex_lock),
    HLE_FN(sys_lwmutex_trylock),
    HLE_FN(sys_lwmutex_unlock),
    HLE_FN(sys_mmapper_allocate_memory),
    HLE_FN(sys_mmapper_free_memory),
    HLE_FN(sys_mmapper_unmap_memory),
    HLE_FN(sys_ppu_thread_create),
    HLE_FN(sys_ppu_thread_exit),
    HLE_FN(sys_ppu_thread_get_id),
    HLE_FN(sys_process_exit),
    HLE_FN(sys_prx_exitspawn_with_level),
    HLE_TODO(sys_raw_spu_load),
    HLE_FN(sys_spinlock_initialize),
    HLE_FN(sys_spinlock_lock),
    HLE_FN(sys_spinlock_trylock),
    HLE_FN(sys_spinlock_unlock),
    HLE_FN(sys_spu_image_close),
    HLE_FN(sys_spu_image_import),
    HLE_FN(sys_time_get_system_time),
});

}  // namespace rt
