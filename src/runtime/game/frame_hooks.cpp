// Hooks on the game's frame presentation (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#include <atomic>
#include <cstring>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/memory.h"
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

// The game's movie frame fetch (0xd30e6c: wraps cellSailGraphicsAdapterGetFrame; its partner
// 0xd30e04 passes cellGcmGetLastFlipTime to cellSailGraphicsAdapterUpdateAvSync). Tells the RSX
// side that a movie is on screen, so flips keep the console's cadence (rsx::note_movie_frame).
void hook_movie_get_frame(Ctx& ctx, void (*orig)(Ctx&)) {
  rt::rsx::note_movie_frame();
  orig(ctx);
}

}  // namespace ppu
