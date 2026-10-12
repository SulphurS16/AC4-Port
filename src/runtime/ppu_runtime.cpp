#include "runtime/ppu_runtime.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <mutex>

#include "common/file.h"
#include "runtime/config.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppu_thread.h"
#include "runtime/reservation.h"

namespace ppu {
struct FuncEntry {
  u32 addr;
  Fn fn;
};
extern const FuncEntry g_funcs[];
extern const size_t g_func_count;
}  // namespace ppu

namespace rt {
namespace res {
Stripe g_stripes[kStripes];
std::atomic<u32> g_seq{0}, g_waiters{0};

void notify_slow() {
  g_seq.fetch_add(1, std::memory_order_seq_cst);
  WakeByAddressAll(&g_seq);
}

std::atomic<int> g_watch_ranges{0};
namespace {
struct WatchEntry {
  double t;  // log time
  u32 ea, thread;
  char kind;
  bool ok;
  u8 n;  // changed doublewords recorded
  u8 off[4];
  u64 before[4], after[4];
};
std::vector<std::pair<u32, u32>> g_watch;  // [begin, end)
std::mutex g_watch_mu;
std::vector<WatchEntry> g_watch_ring;
u64 g_watch_pos = 0;
}  // namespace

bool watched_slow(u32 line) {
  for (auto [b, e] : g_watch)
    if (line + 128 > b && line < e) return true;
  return false;
}
void watch_add(u32 begin, u32 size) {
  if (g_watch_ring.empty()) g_watch_ring.resize(1u << 20);
  g_watch.push_back({begin, begin + size});
  g_watch_ranges.store(int(g_watch.size()));
}
void watch_record(char kind, u32 thread, u32 ea, bool ok, const u8* before, const u8* after) {
  WatchEntry e{};
  e.t = log_time();
  e.ea = ea;
  e.thread = thread;
  e.kind = kind;
  e.ok = ok;
  if (before && after)
    for (int k = 0; k < 16 && e.n < 4; ++k) {
      u64 a, b;
      std::memcpy(&a, before + 8 * k, 8);
      std::memcpy(&b, after + 8 * k, 8);
      if (a != b) {
        e.off[e.n] = u8(8 * k);
        e.before[e.n] = __builtin_bswap64(a);
        e.after[e.n] = __builtin_bswap64(b);
        ++e.n;
      }
    }
  std::lock_guard lk(g_watch_mu);
  g_watch_ring[g_watch_pos++ % g_watch_ring.size()] = e;
}
void watch_dump(const std::string& path) {
  std::lock_guard lk(g_watch_mu);
  if (!g_watch_pos) return;
  try {
    ac4::guard_write(path);
  } catch (const std::exception& e) {
    LOG_E("dump", "%s", e.what());
    return;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    LOG_W("dump", "cannot write %s", path.c_str());
    return;
  }
  u64 n = std::min<u64>(g_watch_pos, g_watch_ring.size());
  std::fprintf(f, "watched atomics: last %llu of %llu\n", (unsigned long long)n, (unsigned long long)g_watch_pos);
  for (u64 i = g_watch_pos - n; i < g_watch_pos; ++i) {
    const WatchEntry& e = g_watch_ring[i % g_watch_ring.size()];
    std::fprintf(f, "%9.6f %c %s t%x %08x", e.t, e.kind, e.ok ? "ok" : "FAIL", e.thread, e.ea);
    for (int k = 0; k < e.n; ++k)
      std::fprintf(f, " +%02x:%016llx->%016llx", e.off[k], (unsigned long long)e.before[k], (unsigned long long)e.after[k]);
    std::fputc('\n', f);
  }
  std::fclose(f);
  LOG_I("dump", "watched atomics: %llu operations written to %s", (unsigned long long)n, path.c_str());
}

void wait(u32 seq, u64 timeout_us) {
  if (g_seq.load(std::memory_order_seq_cst) != seq) return;
  if (timeout_us < 1000) {  // below the wait granularity: yield instead
    SwitchToThread();
    return;
  }
  u32 expected = seq;
  WaitOnAddress(&g_seq, &expected, sizeof(expected), DWORD(std::min<u64>(timeout_us / 1000, 100)));
}
}  // namespace res
namespace {

u32 g_tab_base = 0, g_tab_end = 0;
ppu::Fn* g_tab = nullptr;
std::vector<HleImport> g_imports;
std::mutex g_imp_mu;
std::vector<std::pair<uintptr_t, u32>> g_host_sorted;  // host fn address -> guest address
LARGE_INTEGER g_qpf, g_qpc0;

void dump_ctx(const ppu::Ctx& c, std::string& s) {
  for (int i = 0; i < 32; i += 4)
    s += ac4::strprintf("  r%-2d %016llx r%-2d %016llx r%-2d %016llx r%-2d %016llx\n", i, (unsigned long long)c.r[i], i + 1,
                         (unsigned long long)c.r[i + 1], i + 2, (unsigned long long)c.r[i + 2], i + 3, (unsigned long long)c.r[i + 3]);
  s += ac4::strprintf("  lr %08llx ctr %016llx cr %08x xer so=%u ov=%u ca=%u cia %08x\n", (unsigned long long)c.lr,
                       (unsigned long long)c.ctr, ppu::mfcr(c), c.so, c.ov, c.ca, c.cia);
}

std::string thread_desc() {
  PpuThread* t = cur_thread();
  return t ? ac4::strprintf("thread 0x%x '%s'", t->id, t->name.c_str()) : std::string("non-guest thread");
}

LONG CALLBACK fault_handler(PEXCEPTION_POINTERS ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
    return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t rip = uintptr_t(ep->ContextRecord->Rip);
  std::string s = ac4::strprintf("host exception 0x%08lx at rip %p in %s", code, (void*)rip, thread_desc().c_str());
  HMODULE mod = nullptr;  // module-relative address for symbolization (llvm-symbolizer --obj=<module> 0x...)
  char mod_name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(rip), &mod) && mod) {
    GetModuleFileNameA(mod, mod_name, sizeof mod_name);
    const char* base = std::strrchr(mod_name, '\\');
    s += ac4::strprintf(" (%s+0x%llx)", base ? base + 1 : mod_name, (unsigned long long)(rip - uintptr_t(mod)));
  }
  if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
    uintptr_t a = uintptr_t(ep->ExceptionRecord->ExceptionInformation[1]);
    const char* kind = ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read";
    uintptr_t b = uintptr_t(PPU_MEM_BASE);
    if (a >= b && a < b + 0x100000000ull) {
      u32 ga = u32(a - b);
      s += ac4::strprintf("\n  guest %s at 0x%08x (%s)", kind, ga, vm::describe(ga).c_str());
    } else {
      s += ac4::strprintf("\n  host %s at %p", kind, (void*)a);
    }
  }
  // Which recompiled function was executing?
  if (!g_host_sorted.empty()) {
    auto it = std::upper_bound(g_host_sorted.begin(), g_host_sorted.end(), std::make_pair(rip, ~0u));
    if (it != g_host_sorted.begin()) {
      --it;
      s += ac4::strprintf("\n  in recompiled f_%08x (+0x%llx host bytes)", it->second, (unsigned long long)(rip - it->first));
    }
  }
  if (PpuThread* t = cur_thread()) {
    s += "\n";
    dump_ctx(t->ctx, s);
    const ppu::Ctx& c = t->ctx;
    u32 ctr = u32(c.ctr);
    if (ctr >= kHleCodeBase && ctr < kHleOpdBase && (ctr - kHleCodeBase) / 4 < g_imports.size()) {
      const HleImport& imp = g_imports[(ctr - kHleCodeBase) / 4];
      s += ac4::strprintf("  ctr: HLE import %s::%s\n", imp.module.c_str(), imp.name.c_str());
    }
    s += "  guest backtrace:" + guest_backtrace(c) + "\n";
  }
  fatal(s);
}

}  // namespace

void ppu_runtime_init() {
  // HLE area for fake import descriptors (must exist before any module binds imports).
  if (!vm::alloc_fixed(vm::Area::Main, kHleCodeBase, kHleAreaEnd - kHleCodeBase)) fatal("cannot map HLE area");
  QueryPerformanceFrequency(&g_qpf);
  QueryPerformanceCounter(&g_qpc0);
  u32 lo = ~0u, hi = 0;
  for (size_t i = 0; i < ppu::g_func_count; ++i) {
    lo = std::min(lo, ppu::g_funcs[i].addr);
    hi = std::max(hi, ppu::g_funcs[i].addr);
  }
  g_tab_base = lo;
  g_tab_end = hi + 4;
  size_t n = (g_tab_end - g_tab_base) / 4;
  g_tab = static_cast<ppu::Fn*>(VirtualAlloc(nullptr, n * sizeof(ppu::Fn), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!g_tab) fatal("cannot allocate function table");
  g_host_sorted.reserve(ppu::g_func_count);
  for (size_t i = 0; i < ppu::g_func_count; ++i) {
    g_tab[(ppu::g_funcs[i].addr - g_tab_base) / 4] = ppu::g_funcs[i].fn;
    g_host_sorted.push_back({reinterpret_cast<uintptr_t>(ppu::g_funcs[i].fn), ppu::g_funcs[i].addr});
  }
  std::sort(g_host_sorted.begin(), g_host_sorted.end());
  LOG_I("ppu", "%zu recompiled functions, table 0x%08x-0x%08x", ppu::g_func_count, g_tab_base, g_tab_end);
}

ppu::Fn find_function(u32 a) {
  if (a < g_tab_base || a >= g_tab_end || (a & 3)) return nullptr;
  return g_tab[(a - g_tab_base) / 4];
}

std::string guest_backtrace(const ppu::Ctx& c) {
  // Guest back chain: 0(r1) links to the caller's frame, whose 16(...) holds the saved LR.
  std::string s;
  u32 sp = u32(c.r[1]);
  for (int depth = 0; depth < 12 && sp >= 0xD0000000u && sp < 0xF0000000u && !(sp & 7); ++depth) {
    u32 next = u32(ppu::r64(sp));
    if (next <= sp || next >= 0xF0000000u) break;
    s += ac4::strprintf(" %08x", u32(ppu::r64(next + 16)));
    sp = next;
  }
  return s;
}

std::string guest_symbol(u32 a) { return ac4::strprintf("0x%08x", a); }

u32 guest_function_at(uintptr_t host, bool exact) {
  auto it = std::upper_bound(g_host_sorted.begin(), g_host_sorted.end(), std::make_pair(host, ~0u));
  if (it == g_host_sorted.begin()) return 0;
  --it;
  return !exact || it->first == host ? it->second : 0;
}

u32 hle_register_import(const HleImport& imp) {
  std::lock_guard lk(g_imp_mu);
  u32 i = u32(g_imports.size());
  if (kHleOpdBase + 8 * (i + 1) > kHleAreaEnd || kHleCodeBase + 4 * (i + 1) > kHleOpdBase) fatal("HLE import table full");
  g_imports.push_back(imp);
  u32 opd = kHleOpdBase + 8 * i;
  ppu::w32(opd, kHleCodeBase + 4 * i);
  ppu::w32(opd + 4, 0);
  return opd;
}

u32 hle_internal_opd(const char* name, hle::Handler fn) {
  HleImport imp;
  imp.module = "ac4_internal";
  imp.name = name;
  imp.fn = fn;
  return hle_register_import(imp);
}

void dump_hle_stats() {
  std::lock_guard lk(g_imp_mu);
  std::string s = "HLE call counts:\n";
  for (const auto& i : g_imports)
    if (i.calls) s += ac4::strprintf("  %10llu %s::%s\n", (unsigned long long)i.calls, i.module.c_str(), i.name.c_str());
  LOG_I("hle", "%s", s.c_str());
}

void install_fault_handler() { AddVectoredExceptionHandler(1, fault_handler); }

}  // namespace rt

// ---------------------------------------------------------------------------------------------
// Hooks called from generated code.
namespace ppu {

using namespace rt;

void icall(Ctx& ctx, u32 target) {
  if (Fn f = find_function(target)) {
    f(ctx);
    return;
  }
  if (target >= kHleCodeBase && target < kHleOpdBase && !(target & 3)) {
    u32 i = (target - kHleCodeBase) / 4;
    HleImport* imp;
    {
      std::lock_guard lk(g_imp_mu);
      if (i >= g_imports.size()) fatal(ac4::strprintf("bad HLE code address 0x%08x", target));
      imp = &g_imports[i];
    }
    ++imp->calls;
    if (imp->lle_opd) {  // traced call into a firmware module: the callee runs with its own TOC
      LOG_I("lle", "%s::%s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) lr=0x%08llx in %s", imp->module.c_str(), imp->name.c_str(),
            (unsigned long long)ctx.r[3], (unsigned long long)ctx.r[4], (unsigned long long)ctx.r[5], (unsigned long long)ctx.r[6],
            (unsigned long long)ctx.r[7], (unsigned long long)ctx.lr, thread_desc().c_str());
      ctx.r[2] = ppu::r32(imp->lle_opd + 4);
      u32 code = ppu::r32(imp->lle_opd);
      Fn f = find_function(code);
      if (!f) fatal(ac4::strprintf("traced LLE import %s::%s has no code at 0x%08x", imp->module.c_str(), imp->name.c_str(), code));
      f(ctx);
      LOG_I("lle", "  %s -> 0x%llx", imp->name.c_str(), (unsigned long long)ctx.r[3]);
      return;
    }
    if (!imp->fn && cfg().lenient) {
      LOG_E("lenient", "UNIMPLEMENTED %s::%s called (lr 0x%08llx) -> returning 0 [survey mode]", imp->module.c_str(), imp->name.c_str(), (unsigned long long)ctx.lr);
      ctx.r[3] = 0;
      return;
    }
    if (!imp->fn) {
      std::string s = ac4::strprintf("unimplemented HLE function %s::%s (fnid 0x%08x) called from 0x%08x (lr 0x%08llx) in %s",
                                      imp->module.c_str(), imp->name.c_str(), imp->fnid, ctx.cia, (unsigned long long)ctx.lr,
                                      thread_desc().c_str());
      s += "\n";
      dump_ctx(ctx, s);
      fatal(s);
    }
    if (hle::g_trace)
      LOG_I("hle", "%s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) lr=0x%08llx", imp->name.c_str(), (unsigned long long)ctx.r[3],
            (unsigned long long)ctx.r[4], (unsigned long long)ctx.r[5], (unsigned long long)ctx.r[6], (unsigned long long)ctx.r[7],
            (unsigned long long)ctx.lr);
    {
      CallScope scope(imp->name.c_str(), ctx);
      imp->fn(ctx);
    }
    if (hle::g_trace) LOG_I("hle", "  %s -> 0x%llx", imp->name.c_str(), (unsigned long long)ctx.r[3]);
    return;
  }
  std::string s = ac4::strprintf("call to unknown guest address 0x%08x from 0x%08x (lr 0x%08llx) in %s\n", target, ctx.cia,
                                  (unsigned long long)ctx.lr, thread_desc().c_str());
  dump_ctx(ctx, s);
  fatal(s);
}

void syscall(Ctx& ctx) {
  u32 n = u32(ctx.r[11]);
  const hle::Syscall* e = hle::syscall_entry(n);
  if (!e && cfg().lenient) {
    LOG_E("lenient", "UNIMPLEMENTED syscall %u called at 0x%08x -> returning 0 [survey mode]", n, ctx.cia);
    ctx.r[3] = 0;
    return;
  }
  if (!e) {
    std::string s = ac4::strprintf("unimplemented syscall %u at 0x%08x (lr 0x%08llx) in %s\n", n, ctx.cia, (unsigned long long)ctx.lr,
                                    thread_desc().c_str());
    dump_ctx(ctx, s);
    fatal(s);
  }
  if (hle::g_trace)
    LOG_I("sys", "%s(0x%llx, 0x%llx, 0x%llx, 0x%llx) at 0x%08x", e->name, (unsigned long long)ctx.r[3], (unsigned long long)ctx.r[4],
          (unsigned long long)ctx.r[5], (unsigned long long)ctx.r[6], ctx.cia);
  {
    CallScope scope(e->name, ctx);
    e->fn(ctx);
  }
  if (hle::g_trace) LOG_I("sys", "  %s -> 0x%llx", e->name, (unsigned long long)ctx.r[3]);
}

void trap(Ctx& ctx, u32 pc, const char* why) {
  std::string s = ac4::strprintf("guest trap '%s' at 0x%08x in %s\n", why, pc, thread_desc().c_str());
  dump_ctx(ctx, s);
  fatal(s);
}

void bad_switch(Ctx& ctx, u32 pc, u32 target) {
  std::string s = ac4::strprintf("switch at 0x%08x jumped to unexpected target 0x%08x in %s\n", pc, target, thread_desc().c_str());
  dump_ctx(ctx, s);
  fatal(s);
}

u64 timebase() {
  // Cell timebase runs at 79.8 MHz.
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  u64 ticks = u64(now.QuadPart - g_qpc0.QuadPart);
  u64 q = u64(g_qpf.QuadPart);
  return (ticks / q) * 79800000ull + (ticks % q) * 79800000ull / q;
}

u64 read_spr(Ctx& ctx, u32 pc, u32 spr) {
  trap(ctx, pc, ac4::strprintf("mfspr %u", spr).c_str());
}

void write_spr(Ctx& ctx, u32 pc, u32 spr, u64) { trap(ctx, pc, ac4::strprintf("mtspr %u", spr).c_str()); }

namespace {
// lwarx/ldarx: the value and a copy of its 128-byte reservation granule, read under the line lock
// so no other atomic line update (PPU or SPU) is seen half done.
template <typename T>
T load_reserved(Ctx& ctx, u32 ea) {
  u32 line = ea & ~127u;
  {
    rt::res::LineLock lk(line);
    std::memcpy(ctx.res_line, mem() + line, 128);
  }
  T v;
  std::memcpy(&v, ctx.res_line + (ea & 127u), sizeof(T));
  v = sizeof(T) == 4 ? T(__builtin_bswap32(u32(v))) : T(__builtin_bswap64(u64(v)));
  ctx.res_addr = ea;
  ctx.res_size = sizeof(T);
  ctx.res_val = v;
  return v;
}

// stwcx/stdcx: stores only if the reservation is held and nothing in its granule changed since
// the reserving load (other words included: lock-free structures such as SPURS queues pack the
// PPU's and the SPU's indices into one line and depend on this). The compare-and-swap of the word
// itself also catches plain stores racing with the check.
std::atomic<u64> g_granule_conflicts{0};  // stores refused only because another word of the line changed

template <typename T>
bool store_conditional(Ctx& ctx, u32 ea, T v) {
  bool have = ctx.res_addr == ea && ctx.res_size == sizeof(T);
  ctx.res_addr = 0;
  if (!have) return false;
  u32 line = ea & ~127u;
  T expected = sizeof(T) == 4 ? T(__builtin_bswap32(u32(ctx.res_val))) : T(__builtin_bswap64(u64(ctx.res_val)));
  T desired = sizeof(T) == 4 ? T(__builtin_bswap32(u32(v))) : T(__builtin_bswap64(u64(v)));
  bool ok;
  {
    rt::res::LineLock lk(line);
    bool watch = rt::res::watched(line);
    alignas(8) u8 before[128];
    if (watch) std::memcpy(before, mem() + line, 128);
    if (std::memcmp(mem() + line, ctx.res_line, 128) == 0) {
      ok = __atomic_compare_exchange_n(reinterpret_cast<T*>(mem() + ea), &expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
    } else {
      ok = false;
      T cur;
      std::memcpy(&cur, mem() + ea, sizeof(T));
      if (cur == expected) g_granule_conflicts.fetch_add(1, std::memory_order_relaxed);
    }
    if (watch) {
      rt::PpuThread* t = rt::cur_thread();
      rt::res::watch_record('C', t ? t->id : 0, ea, ok, ok ? before : ctx.res_line, mem() + line);
    }
  }
  if (ok) rt::res::notify();
  return ok;
}
}  // namespace

u64 granule_conflicts() { return g_granule_conflicts.load(); }
u32 lwarx32(Ctx& ctx, u32 ea) { return load_reserved<u32>(ctx, ea); }
u64 ldarx64(Ctx& ctx, u32 ea) { return load_reserved<u64>(ctx, ea); }
bool stcx32(Ctx& ctx, u32 ea, u32 v) { return store_conditional<u32>(ctx, ea, v); }
bool stcx64(Ctx& ctx, u32 ea, u64 v) { return store_conditional<u64>(ctx, ea, v); }

}  // namespace ppu

namespace ppu {
// Diagnostics hook for temporarily instrumented generated code (declare it locally there):
// logs a tag, a value, the argument registers and the guest call chain of the calling thread.
// Timing variant: phase 0 starts a per-thread stopwatch, phase 1 logs if more than
// `threshold_ms` passed since.
void diag_time(const char* tag, int phase, double threshold_ms) {
  thread_local double t0 = 0;
  if (phase == 0) {
    t0 = rt::log_time();
  } else if (double dt = rt::log_time() - t0; dt * 1000 > threshold_ms) {
    LOG_I("diag", "%s took %.0f ms in %s", tag, dt * 1000, rt::thread_desc().c_str());
  }
}
void diag(const char* tag, Ctx& ctx, u32 v) {
  LOG_I("diag", "%s 0x%08x (r3 0x%08llx r4 0x%08llx lr 0x%08llx) in %s, backtrace%s", tag, v, (unsigned long long)ctx.r[3],
        (unsigned long long)ctx.r[4], (unsigned long long)ctx.lr, rt::thread_desc().c_str(), rt::guest_backtrace(ctx).c_str());
}
}  // namespace ppu
