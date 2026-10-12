// Guest (PS3 user process) address space: 4 GiB reserved at PPU_MEM_BASE, committed on demand.
//   0x00010000-0x0FFFFFFF  main: ELF image, then runtime-owned allocations (TLS, heaps, HLE data)
//   0x10000000-0x1FFFFFFF  main (continued)
//   0x20000000-0x2FFFFFFF  user memory, 64 KiB pages (sys_memory_allocate)
//   0x30000000-0x3FFFFFFF  user memory, 1 MiB pages (sys_memory_allocate)
//   0x40000000-0xBFFFFFFF  sys_mmapper address areas (256 MiB granularity)
//   0xC0000000-0xCFFFFFFF  RSX local memory
//   0xD0000000-0xDFFFFFFF  thread stacks (guard page below each)
//   0xE0000000-0xFFFFFFFF  SPU local storage / problem-state mappings
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::vm {

enum class Area { Main, User64k, User1m, Mmapper, Video, Stack, Spu };

bool init();
// Guest pages of [begin, begin + size) written since the previous call for them (page-aligned
// guest addresses, appended to `out`), from the OS's write tracking of the guest reservation;
// false if write tracking is unavailable.
bool take_written_pages(u32 begin, u32 size, std::vector<u32>& out);
u8* base();

void commit(u32 addr, u32 size);
void decommit(u32 addr, u32 size);
bool committed(u32 addr, u32 size = 1);

// Allocates (and commits) `size` bytes in an area. Returns 0 on failure.
u32 alloc(Area a, u32 size, u32 align = 0x1000);
// Allocates at a fixed address (fails if it overlaps an existing allocation).
bool alloc_fixed(Area a, u32 addr, u32 size);
// Frees an allocation made with alloc/alloc_fixed. Returns the freed size (0 if not found).
u32 free(Area a, u32 addr);
// Marks [begin, end) of an area as in use without committing (e.g. the ELF image).
void reserve_range(Area a, u32 begin, u32 end);

struct AreaStats {
  u32 begin, end;
  u64 used;
};
AreaStats stats(Area a);

std::string describe(u32 addr);  // which area/allocation an address falls in (diagnostics)
// Calls fn(begin, size) for each committed allocation of an area (a snapshot; diagnostics).
void for_each_block(Area a, const std::function<void(u32, u32)>& fn);

}  // namespace rt::vm
