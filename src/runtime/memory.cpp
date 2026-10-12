#include "runtime/memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/ppu_runtime.h"
#include "runtime/ppu_thread.h"

namespace rt::vm {
namespace {

struct AreaInfo {
  u32 begin, end;  // [begin, end)
  std::map<u32, u32> blocks;  // addr -> size
  u64 used = 0;
};

std::mutex g_mu;
AreaInfo g_areas[] = {
    {0x00010000, 0x20000000, {}, 0},  // Main
    {0x20000000, 0x30000000, {}, 0},  // User64k
    {0x30000000, 0x40000000, {}, 0},  // User1m
    {0x40000000, 0xC0000000, {}, 0},  // Mmapper
    {0xC0000000, 0xD0000000, {}, 0},  // Video
    {0xD0000000, 0xE0000000, {}, 0},  // Stack
    {0xE0000000, 0xFFFF0000, {}, 0},  // Spu
};
AreaInfo& area(Area a) { return g_areas[int(a)]; }

// Provenance for diagnosing accesses to freed memory: who allocated each block, and the most
// recent frees (a fault in unallocated memory reports the free that released the address).
struct Origin {
  double t = 0;
  std::string thread, backtrace;
};
Origin origin_now() {
  Origin o;
  o.t = log_time();
  if (PpuThread* t = cur_thread()) {
    o.thread = t->name;
    o.backtrace = guest_backtrace(t->ctx);
  } else {
    o.thread = "host";
  }
  return o;
}
struct Freed {
  u32 addr = 0, size = 0;
  Origin alloc, freed;
};
std::map<u32, Origin> g_alloc_origin;  // block address -> allocation origin
std::vector<Freed> g_freed(8192);      // ring of recent frees
size_t g_freed_next = 0;
std::string origin_text(const Origin& o) {
  return ac4::strprintf("%.3f s by '%s' (guest backtrace%s)", o.t, o.thread.c_str(), o.backtrace.c_str());
}
const char* area_name(int i) {
  static const char* n[] = {"main", "user64k", "user1m", "mmapper", "video", "stack", "spu"};
  return n[i];
}

}  // namespace

u8* base() { return reinterpret_cast<u8*>(PPU_MEM_BASE); }

bool g_write_watch = false;

bool init() {
  // Write tracking lets the renderer notice changed textures without hashing them every frame.
  void* p = VirtualAlloc(base(), 0x100000000ull, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
  g_write_watch = p == base();
  if (!g_write_watch) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    p = VirtualAlloc(base(), 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    LOG_W("vm", "guest write tracking unavailable; textures are re-checked by hashing");
  }
  if (p != base()) {
    LOG_E("vm", "cannot reserve guest address space at %p (got %p, error %lu)", (void*)base(), p, GetLastError());
    return false;
  }
  return true;
}

bool take_written_pages(u32 begin, u32 size, std::vector<u32>& out) {
  if (!g_write_watch) return false;
  static std::vector<void*> buf;
  if (buf.size() < size / 0x1000 + 1) buf.resize(size / 0x1000 + 1);  // every page fits: one call
  ULONG_PTR count = buf.size();
  DWORD gran = 0;
  if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base() + begin, size, buf.data(), &count, &gran) != 0) return false;
  for (ULONG_PTR i = 0; i < count; ++i) out.push_back(u32(static_cast<u8*>(buf[i]) - base()));
  return true;
}

void commit(u32 addr, u32 size) {
  if (!size) return;
  u64 a = addr & ~0xfffull, e = (u64(addr) + size + 0xfff) & ~0xfffull;
  if (!VirtualAlloc(base() + a, size_t(e - a), MEM_COMMIT, PAGE_READWRITE))
    fatal(ac4::strprintf("guest commit failed at 0x%08x size 0x%x (error %lu)", addr, size, GetLastError()));
}

void decommit(u32 addr, u32 size) {
  if (!size) return;
  u64 a = (u64(addr) + 0xfff) & ~0xfffull, e = (u64(addr) + size) & ~0xfffull;
  if (e > a) VirtualFree(base() + a, size_t(e - a), MEM_DECOMMIT);
}

bool committed(u32 addr, u32 size) {
  u64 a = addr, e = u64(addr) + (size ? size : 1);
  while (a < e) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base() + a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    u64 region_end = u64(reinterpret_cast<u8*>(mbi.BaseAddress) - base()) + mbi.RegionSize;
    a = region_end;
  }
  return true;
}

u32 alloc(Area ar, u32 size, u32 align) {
  if (!size) return 0;
  if (align < 0x1000) align = 0x1000;
  size = (size + 0xfff) & ~0xfffu;
  std::lock_guard lk(g_mu);
  AreaInfo& A = area(ar);
  // Stacks get an uncommitted guard page below them.
  u32 guard = ar == Area::Stack ? 0x1000 : 0;
  u64 cur = (u64(A.begin) + align - 1) & ~u64(align - 1);
  for (auto it = A.blocks.begin();; ++it) {
    u64 cand = (cur + guard + align - 1) & ~u64(align - 1);
    u64 limit = it == A.blocks.end() ? A.end : it->first;
    if (cand + size <= limit) {
      u32 addr = u32(cand);
      A.blocks[addr] = size;
      A.used += size;
      g_alloc_origin[addr] = origin_now();
      commit(addr, size);
      std::memset(base() + addr, 0, size);
      return addr;
    }
    if (it == A.blocks.end()) break;
    cur = u64(it->first) + it->second;
  }
  LOG_E("vm", "alloc of 0x%x bytes (align 0x%x) failed in %s", size, align, area_name(int(ar)));
  return 0;
}

bool alloc_fixed(Area ar, u32 addr, u32 size) {
  size = (size + 0xfff) & ~0xfffu;
  std::lock_guard lk(g_mu);
  AreaInfo& A = area(ar);
  if (addr < A.begin || u64(addr) + size > A.end) return false;
  auto it = A.blocks.upper_bound(addr);
  if (it != A.blocks.end() && it->first < addr + size) return false;
  if (it != A.blocks.begin()) {
    auto p = std::prev(it);
    if (u64(p->first) + p->second > addr) return false;
  }
  A.blocks[addr] = size;
  A.used += size;
  g_alloc_origin[addr] = origin_now();
  commit(addr, size);
  return true;
}

void reserve_range(Area ar, u32 b, u32 e) {
  std::lock_guard lk(g_mu);
  AreaInfo& A = area(ar);
  u32 size = ((e - b) + 0xfff) & ~0xfffu;
  A.blocks[b & ~0xfffu] = size;
  A.used += size;
}

u32 free(Area ar, u32 addr) {
  std::lock_guard lk(g_mu);
  AreaInfo& A = area(ar);
  auto it = A.blocks.find(addr);
  if (it == A.blocks.end()) return 0;
  u32 size = it->second;
  decommit(addr, size);
  A.used -= size;
  A.blocks.erase(it);
  Freed& f = g_freed[g_freed_next++ % g_freed.size()];
  f.addr = addr;
  f.size = size;
  auto o = g_alloc_origin.find(addr);
  f.alloc = o != g_alloc_origin.end() ? o->second : Origin{};
  if (o != g_alloc_origin.end()) g_alloc_origin.erase(o);
  f.freed = origin_now();
  return size;
}

void for_each_block(Area ar, const std::function<void(u32, u32)>& fn) {
  std::vector<std::pair<u32, u32>> blocks;
  {
    std::lock_guard lk(g_mu);
    for (auto& [a, s] : area(ar).blocks) blocks.push_back({a, s});
  }
  for (auto [a, s] : blocks)
    if (committed(a, s)) fn(a, s);
}

AreaStats stats(Area ar) {
  std::lock_guard lk(g_mu);
  const AreaInfo& A = area(ar);
  return {A.begin, A.end, A.used};
}

std::string describe(u32 addr) {
  std::lock_guard lk(g_mu);
  for (int i = 0; i < int(std::size(g_areas)); ++i) {
    const AreaInfo& A = g_areas[i];
    if (addr < A.begin || addr >= A.end) continue;
    auto it = A.blocks.upper_bound(addr);
    if (it != A.blocks.begin()) {
      auto p = std::prev(it);
      if (addr < u64(p->first) + p->second)
        return ac4::strprintf("%s block 0x%08x+0x%x (offset 0x%x)", area_name(i), p->first, p->second, addr - p->first);
    }
    std::string s = ac4::strprintf("%s (unallocated)", area_name(i));
    for (size_t k = 1; k <= g_freed.size() && k <= g_freed_next; ++k) {  // newest first
      const Freed& f = g_freed[(g_freed_next - k) % g_freed.size()];
      if (addr < f.addr || addr >= u64(f.addr) + f.size) continue;
      s += ac4::strprintf("\n  was block 0x%08x+0x%x (offset 0x%x), freed at %s\n  allocated at %s", f.addr, f.size,
                           addr - f.addr, origin_text(f.freed).c_str(), origin_text(f.alloc).c_str());
      break;
    }
    return s;
  }
  return "outside all areas";
}

}  // namespace rt::vm
