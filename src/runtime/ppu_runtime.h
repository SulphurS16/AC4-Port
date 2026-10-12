// Runtime side of recompiled PPU code: function lookup, HLE import table, diagnostics.
#pragma once
#include <string>
#include <vector>

#include "ppu/ppu_recomp.h"
#include "runtime/hle.h"

namespace ppu {
// Conditional stores refused although the reserved word itself was unchanged (another part of its
// 128-byte reservation granule changed); diagnostics.
u64 granule_conflicts();
}  // namespace ppu

namespace rt {

constexpr u32 kHleCodeBase = 0x0FF00000;  // fake code addresses for HLE functions (4 bytes each)
constexpr u32 kHleOpdBase = 0x0FF80000;   // fake OPDs {code, toc} (8 bytes each)
constexpr u32 kHleAreaEnd = 0x10000000;

struct HleImport {
  std::string module, name;
  u32 fnid = 0;
  hle::Handler fn = nullptr;
  u32 lle_opd = 0;  // traced forward to a natively run firmware function (--trace-lle)
  u64 calls = 0;
};

void ppu_runtime_init();                       // builds the guest-address -> host function table
u32 hle_register_import(const HleImport& imp);  // returns the fake OPD address
// Runtime-internal HLE function callable by guest code through an OPD (e.g. gcm context callback).
u32 hle_internal_opd(const char* name, hle::Handler fn);
ppu::Fn find_function(u32 guest_addr);
std::string guest_symbol(u32 guest_addr);      // "f_XXXXXXXX+0x.." best effort
// The guest address of the recompiled function whose host code starts at `host` (exact), or that
// most likely contains it (the nearest one starting before it); 0 if none.
u32 guest_function_at(uintptr_t host, bool exact);
std::string guest_backtrace(const ppu::Ctx& ctx);  // " lr1 lr2 ..." from the stack back chain
void install_fault_handler();
void dump_hle_stats();

}  // namespace rt
