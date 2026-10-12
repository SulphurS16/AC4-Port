// Hooks on the game's Lua 5.0 runtime (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#include <atomic>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"
#include "runtime/memory.h"

namespace ppu {

// luaD_throw(L, status) at 0x2b12dc: a script error on its way to the protected call (the
// game's longjmp; runtime/guest_setjmp.cpp). Logs the error message: the error object is at
// L->top - 1 (L +8; TObject {tt, value}, 8 bytes; a string is tt 4, its text at TString +16).
void hook_lua_throw(Ctx& ctx, void (*orig)(Ctx&)) {
  static std::atomic<u32> count{0};
  u32 n = count.fetch_add(1) + 1;
  if (n <= 50 || n % 100 == 0) {
    u32 L = u32(ctx.r[3]), status = u32(ctx.r[4]);
    std::string msg = "?";
    if (rt::vm::committed(L, 16)) {
      u32 top = r32(L + 8);
      if (top >= 8 && rt::vm::committed(top - 8, 8)) {
        u32 tt = r32(top - 8), v = r32(top - 4);
        if (tt == 4 && rt::vm::committed(v + 16, 1)) {
          msg.clear();
          for (u32 i = 0; i < 400 && rt::vm::committed(v + 16 + i, 1); ++i) {
            char c = char(r8(v + 16 + i));
            if (!c) break;
            msg += (c == '\n' || c == '\r') ? ' ' : c;
          }
        } else {
          msg = ac4::strprintf("(error object of type %u)", tt);
        }
      }
    }
    LOG_W("lua", "script error %u (status %u, from 0x%08x): %s", n, status, u32(ctx.lr), msg.c_str());
  }
  orig(ctx);
}

}  // namespace ppu
