// LV2 user memory: sys_memory_*, memory containers and sys_mmapper_*.
#include <set>
#include <vector>

#include "runtime/lv2/lv2.h"
#include "runtime/lv2/memory.h"
#include "runtime/memory.h"

namespace rt::lv2 {
namespace {

constexpr u64 PAGE_64K = 0x200, PAGE_1M = 0x400, PAGE_MASK = 0xf00;

std::mutex g_mu;
u32 g_total = 0xD500000;  // default container; set by memory_init from the SDK version
u64 g_used = 0;
std::map<u32, std::pair<u32, vm::Area>> g_allocs;  // sys_memory_allocate: addr -> (size, area)

struct Container {
  u32 size = 0;
  u64 used = 0;
};
IdMap<Container> g_containers(0x3f000000);

struct MemObj {
  u32 size = 0, page = 0;
  u32 mapped = 0;          // guest address while mapped
  std::vector<u8> backup;  // contents while unmapped
};
IdMap<MemObj> g_memobj(0x08000000);
std::set<u32> g_mm_areas;                  // reserved 256 MiB areas (start addresses)
std::map<u32, u32> g_mm_area_size;         // start -> size
std::map<u32, std::pair<u32, u32>> g_maps; // mapped addr -> (size, mem id)

s32 take(u64 n) {
  if (g_used + n > g_total) return CELL_ENOMEM;
  g_used += n;
  return CELL_OK;
}

s32 sys_memory_allocate(u32 size, u64 flags, Ptr<be32> out) {
  if (!size) return CELL_EALIGN;
  u32 page = (flags & PAGE_MASK) == PAGE_64K ? 0x10000 : 0x100000;
  if ((flags & PAGE_MASK) != PAGE_64K && (flags & PAGE_MASK) != PAGE_1M && (flags & PAGE_MASK) != 0) return CELL_EINVAL;
  if (size % page) return CELL_EALIGN;
  std::lock_guard lk(g_mu);
  if (take(size) != CELL_OK) return CELL_ENOMEM;
  vm::Area area = page == 0x10000 ? vm::Area::User64k : vm::Area::User1m;
  u32 a = vm::alloc(area, size, page);
  if (!a) {
    g_used -= size;
    return CELL_ENOMEM;
  }
  g_allocs[a] = {size, area};
  *out = a;
  LOG_D("mem", "sys_memory_allocate(0x%x, page 0x%x) -> 0x%08x (used 0x%llx/0x%x)", size, page, a, (unsigned long long)g_used, g_total);
  return CELL_OK;
}
s32 sys_memory_allocate_from_container(u32 size, u32 cid, u64 flags, Ptr<be32> out) {
  auto c = g_containers.get(cid);
  if (!c) return CELL_ESRCH;
  u32 page = (flags & PAGE_MASK) == PAGE_64K ? 0x10000 : 0x100000;
  if (!size || size % page) return CELL_EALIGN;
  std::lock_guard lk(g_mu);
  if (c->used + size > c->size) return CELL_ENOMEM;
  vm::Area area = page == 0x10000 ? vm::Area::User64k : vm::Area::User1m;
  u32 a = vm::alloc(area, size, page);
  if (!a) return CELL_ENOMEM;
  c->used += size;
  g_allocs[a] = {size, area};
  *out = a;
  return CELL_OK;
}
s32 sys_memory_free(u32 addr) {
  std::lock_guard lk(g_mu);
  auto it = g_allocs.find(addr);
  if (it == g_allocs.end()) return CELL_EINVAL;
  vm::free(it->second.second, addr);
  g_used -= it->second.first;
  g_allocs.erase(it);
  return CELL_OK;
}
struct PageAttr {
  be64 attribute, access_right;
  be32 page_size, pad;
};
s32 sys_memory_get_page_attribute(u32 addr, Ptr<PageAttr> attr) {
  if (!vm::committed(addr)) return CELL_EINVAL;
  attr->attribute = 0x40000ull;
  attr->access_right = 0xfull;
  u32 ps = 4096;
  {
    std::lock_guard lk(g_mu);
    auto it = g_allocs.upper_bound(addr);
    if (it != g_allocs.begin()) {
      --it;
      if (addr < it->first + it->second.first) ps = it->second.second == vm::Area::User64k ? 0x10000 : 0x100000;
    }
  }
  attr->page_size = ps;
  attr->pad = 0;
  return CELL_OK;
}
struct MemInfo {
  be32 total, available;
};
s32 sys_memory_get_user_memory_size(Ptr<MemInfo> info) {
  std::lock_guard lk(g_mu);
  u64 others = 0;
  g_containers.for_each([&](u32, Container& c) { others += c.size; });
  info->total = u32(g_total - others);
  info->available = u32(g_total - g_used);
  return CELL_OK;
}
s32 sys_memory_container_create(Ptr<be32> cid, u32 size) {
  if (!size || size % 0x100000) return CELL_EALIGN;
  std::lock_guard lk(g_mu);
  if (take(size) != CELL_OK) return CELL_ENOMEM;
  auto c = std::make_shared<Container>();
  c->size = size;
  *cid = g_containers.add(c);
  return CELL_OK;
}
s32 sys_memory_container_destroy(u32 cid) {
  auto c = g_containers.get(cid);
  if (!c) return CELL_ESRCH;
  std::lock_guard lk(g_mu);
  if (c->used) return CELL_EBUSY;
  g_used -= c->size;
  g_containers.remove(cid);
  return CELL_OK;
}
s32 sys_memory_container_get_size(Ptr<MemInfo> info, u32 cid) {
  auto c = g_containers.get(cid);
  if (!c) return CELL_ESRCH;
  info->total = c->size;
  info->available = u32(c->size - c->used);
  return CELL_OK;
}

// ---- mmapper ----
s32 sys_mmapper_allocate_address(u64 size, u64 flags, u64 alignment, Ptr<be32> out) {
  if (!size || size % 0x10000000) return CELL_EALIGN;
  if (alignment && (alignment % 0x10000000)) return CELL_EALIGN;
  std::lock_guard lk(g_mu);
  // 0x40000000-0x4FFFFFFF is the RSX device slot (see rsx.h).
  for (u64 a = 0x50000000; a + size <= 0xC0000000; a += 0x10000000) {
    if (alignment && a % alignment) continue;
    bool free = true;
    for (u32 s : g_mm_areas) {
      u64 e = u64(s) + g_mm_area_size[s];
      if (a < e && a + size > s) free = false;
    }
    if (!free) continue;
    g_mm_areas.insert(u32(a));
    g_mm_area_size[u32(a)] = u32(size);
    *out = u32(a);
    LOG_D("mem", "sys_mmapper_allocate_address(0x%llx) -> 0x%08x", (unsigned long long)size, u32(a));
    return CELL_OK;
  }
  return CELL_ENOMEM;
}
s32 sys_mmapper_free_address(u32 addr) {
  std::lock_guard lk(g_mu);
  if (!g_mm_areas.count(addr)) return CELL_EINVAL;
  u32 end = addr + g_mm_area_size[addr];
  for (auto& [a, m] : g_maps)
    if (a >= addr && a < end) return CELL_EBUSY;
  g_mm_areas.erase(addr);
  g_mm_area_size.erase(addr);
  return CELL_OK;
}
s32 sys_mmapper_search_and_map(u32 start, u32 mem_id, u64 flags, Ptr<be32> out) {
  auto mo = g_memobj.get(mem_id);
  if (!mo) return CELL_ESRCH;
  std::lock_guard lk(g_mu);
  if (mo->mapped) return CELL_EBUSY;
  auto it = g_mm_area_size.find(start);
  if (it == g_mm_area_size.end()) return CELL_EINVAL;
  u32 end = start + it->second;
  for (u64 a = start; a + mo->size <= end; a += mo->page) {
    bool clash = false;
    for (auto& [m, info] : g_maps)
      if (a < u64(m) + info.first && a + mo->size > m) clash = true;
    if (clash) continue;
    if (!vm::alloc_fixed(vm::Area::Mmapper, u32(a), mo->size)) continue;
    if (!mo->backup.empty()) std::memcpy(ppu::mem() + a, mo->backup.data(), mo->size);
    else std::memset(ppu::mem() + a, 0, mo->size);
    mo->backup.clear();
    mo->mapped = u32(a);
    g_maps[u32(a)] = {mo->size, mem_id};
    *out = u32(a);
    LOG_D("mem", "sys_mmapper_search_and_map(area 0x%08x, mem 0x%x size 0x%x) -> 0x%08x", start, mem_id, mo->size, u32(a));
    return CELL_OK;
  }
  return CELL_ENOMEM;
}

hle::SyscallTable s_mem({
    SYSCALL(348, sys_memory_allocate),
    SYSCALL(350, sys_memory_allocate_from_container),
    SYSCALL(349, sys_memory_free),
    SYSCALL(351, sys_memory_get_page_attribute),
    SYSCALL(352, sys_memory_get_user_memory_size),
    SYSCALL(341, sys_memory_container_create),
    SYSCALL(342, sys_memory_container_destroy),
    SYSCALL(343, sys_memory_container_get_size),
    SYSCALL(330, sys_mmapper_allocate_address),
    SYSCALL(331, sys_mmapper_free_address),
    SYSCALL(337, sys_mmapper_search_and_map),
});

}  // namespace

void memory_init(u32 sdk, u64 fixed_bytes) {
  // Default user memory per SDK version (system software behaviour; values as documented by the
  // RPCS3 loader): SDK 1.32 (Armored Core 4) gets 200 MiB.
  u32 total = sdk > 0x0021FFFF ? 0xD500000 : sdk > 0x0019FFFF ? 0xD300000 : sdk > 0x0018FFFF ? 0xD100000
            : sdk > 0x0017FFFF ? 0xD000000 : sdk > 0x00154FFF ? 0xCC00000 : 0xC800000;
  std::lock_guard lk(g_mu);
  g_total = total;
  g_used = fixed_bytes;
  LOG_I("mem", "user memory 0x%x (SDK 0x%x); executable segments and primary stack 0x%llx; 0x%llx available", total, sdk,
        (unsigned long long)fixed_bytes, (unsigned long long)(total - fixed_bytes));
}

void memory_charge(u64 bytes) {
  std::lock_guard lk(g_mu);
  g_used += bytes;
}

s32 mmapper_allocate_memory(u64 size, u64 flags, u32* mem_id) {
  u32 page = (flags & PAGE_MASK) == PAGE_64K ? 0x10000 : 0x100000;
  if (!size || size % page) return CELL_EALIGN;
  std::lock_guard lk(g_mu);
  if (take(size) != CELL_OK) return CELL_ENOMEM;
  auto mo = std::make_shared<MemObj>();
  mo->size = u32(size);
  mo->page = page;
  *mem_id = g_memobj.add(mo);
  return CELL_OK;
}

s32 mmapper_free_memory(u32 mem_id) {
  auto mo = g_memobj.get(mem_id);
  if (!mo) return CELL_ESRCH;
  std::lock_guard lk(g_mu);
  if (mo->mapped) return CELL_EBUSY;
  g_used -= mo->size;
  g_memobj.remove(mem_id);
  return CELL_OK;
}

s32 mmapper_unmap_memory(u32 addr, u32* mem_id) {
  std::lock_guard lk(g_mu);
  auto it = g_maps.find(addr);
  if (it == g_maps.end()) return CELL_EINVAL;
  auto mo = g_memobj.get(it->second.second);
  if (mo) {
    mo->backup.assign(ppu::mem() + addr, ppu::mem() + addr + mo->size);
    mo->mapped = 0;
  }
  vm::free(vm::Area::Mmapper, addr);
  *mem_id = it->second.second;
  g_maps.erase(it);
  return CELL_OK;
}

}  // namespace rt::lv2
