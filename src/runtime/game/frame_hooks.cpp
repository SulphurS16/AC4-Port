// Hooks on the game's frame presentation (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"
#include "runtime/rsx/rsx.h"

namespace ppu {
namespace {

float f32_at(u32 a) {
  u32 w = r32(a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

}  // namespace

// The game's present routine (0x1406b8, r3 = its frame pacer object; the only caller of
// cellGcmSetFlip seen): flips buffer +16 on context +12; in pacer mode 1 (+4) it then measures the
// time since the previous present with the game's timer object (0x128c84) and, when the rate came
// out above the target (float at +32), waits for the flip and flips the same buffer again (the
// frame is shown twice: a cap at the target). +24 holds the last present's timer value.
// (0x141d08 is a second, polling variant with its target at +36; not called in the runs so far.)
// Diagnostics: logs the object whenever its pacing fields change.
void hook_present(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = u32(ctx.r[3]);
  static std::atomic<u64> last_sig{~0ull};
  if (obj && rt::vm::committed(obj, 64)) {
    u32 mode = r32(obj + 4), tbits = r32(obj + 32);
    u64 sig = (u64(mode) << 32) ^ u64(tbits) ^ (u64(obj) << 8);
    if (last_sig.exchange(sig) != sig) {
      std::string words;
      for (u32 i = 0; i < 16; ++i) words += ac4::strprintf(" %08x", r32(obj + 4 * i));
      LOG_I("frame", "present pacer 0x%08x: mode %u, target %g;%s", obj, mode, double(f32_at(obj + 32)), words.c_str());
    }
  }
  orig(ctx);
}

// The game's draw-data arena allocator (0x95c848: r3 arena, r4 bytes; returns the end of the
// allocation, 0x8eb724 peeks at its start). An arena holds 0x30000 16-byte units with its unit
// counter right after them (+0x300000), reset by 0x95c828 / 0x95c67c; nothing checks the bound, so
// an overflow overwrites the counter with draw data and later allocations land anywhere.
// Diagnostics: each arena's fill between resets (a reset shows as the counter going down): the
// peak per cycle and the number of cycles every 5 s, high-water marks (50/75/90%) and overflows
// with the caller. Called by the game's main thread (statistics are not synchronized further).
void hook_arena_alloc(Ctx& ctx, void (*orig)(Ctx&)) {
  constexpr u64 kUnits = 0x30000;
  u32 arena = u32(ctx.r[3]), bytes = u32(ctx.r[4]);
  u32 cur = r32(arena + 0x300000);
  u64 next = u64(cur) + (bytes >> 4) + ((bytes & 15) != 0);
  struct Cycle {
    u32 arena = 0, last = 0, peak = 0, window_peak = 0, cycles = 0, allocs = 0, window_allocs = 0;
    double window_start = 0;
  };
  static Cycle cyc[4];
  Cycle* c = nullptr;
  for (Cycle& x : cyc)
    if (x.arena == arena || !x.arena) {
      c = &x;
      break;
    }
  if (c) {
    if (!c->arena) {
      c->arena = arena;
      c->window_start = rt::log_time();
    }
    if (cur < c->last) {  // reset since the previous allocation: one cycle done
      ++c->cycles;
      c->window_peak = std::max(c->window_peak, c->peak);
      c->window_allocs = std::max(c->window_allocs, c->allocs);
      c->peak = c->allocs = 0;
      double now = rt::log_time();
      if (now - c->window_start >= 5.0) {
        LOG_I("frame", "draw arena 0x%08x: %u cycles in %.1f s, peak %u units (%.1f%%), at most %u allocations per cycle", arena,
              c->cycles, now - c->window_start, c->window_peak, 100.0 * c->window_peak / kUnits, c->window_allocs);
        c->cycles = c->window_peak = c->window_allocs = 0;
        c->window_start = now;
      }
    }
    c->last = u32(std::min<u64>(next, 0xffffffffu));
    c->peak = std::max(c->peak, c->last);
    ++c->allocs;
  }
  int mark = next > kUnits ? 4 : next * 10 > kUnits * 9 ? 3 : next * 4 > kUnits * 3 ? 2 : next * 2 > kUnits ? 1 : 0;
  if (mark) {
    static std::mutex mu;
    static std::map<u32, int> level;  // arena -> highest mark logged
    static int overflows = 0;
    std::lock_guard lk(mu);
    int& lv = level[arena];
    if (mark > lv || (mark == 4 && overflows < 20)) {
      static const char* kName[] = {"", "50%", "75%", "90%", "OVERFLOW"};
      if (mark == 4) ++overflows;
      lv = std::max(lv, mark);
      std::string msg = ac4::strprintf("draw arena 0x%08x %s: %llu of %llu units after %u bytes (from 0x%08x;%s)", arena,
                                       kName[mark], (unsigned long long)next, (unsigned long long)kUnits, bytes, u32(ctx.lr),
                                       rt::guest_backtrace(ctx).c_str());
      if (mark == 4)
        LOG_W("frame", "%s", msg.c_str());
      else
        LOG_I("frame", "%s", msg.c_str());
    }
  }
  orig(ctx);
}

// The game's movie frame fetch (0xd30e6c: wraps cellSailGraphicsAdapterGetFrame; its partner
// 0xd30e04 passes cellGcmGetLastFlipTime to cellSailGraphicsAdapterUpdateAvSync). Tells the RSX
// side that a movie is on screen, so flips keep the console's cadence (rsx::note_movie_frame).
void hook_movie_get_frame(Ctx& ctx, void (*orig)(Ctx&)) {
  rt::rsx::note_movie_frame();
  orig(ctx);
}

}  // namespace ppu
