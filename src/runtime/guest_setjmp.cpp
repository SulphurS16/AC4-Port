// Guest setjmp/longjmp across recompiled code (declarations and design in ppu/ppu_recomp.h).
// The game's Lua 5.0 runtime raises script errors with longjmp back to its protected-call
// setjmp (luaD_rawrunprotected and inlined variants); errors a script handles on the console
// must be handled the same way here.
#include <deque>

#include "ppu/ppu_recomp.h"
#include "runtime/log.h"

namespace ppu {
namespace {

thread_local std::deque<SjEntry> t_sj;  // live setjmp calls of this thread, innermost last
thread_local u32 t_pending_buf = 0;
thread_local u64 t_pending_val = 0;
std::atomic<Fn> g_longjmp_code{nullptr};  // the game's longjmp (guest register restore)

}  // namespace

SjScope::SjScope() : depth(t_sj.size()) {}

SjScope::~SjScope() {
  while (t_sj.size() > depth) t_sj.pop_back();
}

SjEntry* sj_push(Ctx& ctx, SjScope& scope) {
  u32 buf = u32(ctx.r[3]);
  if (!t_sj.empty() && t_sj.back().scope == &scope && t_sj.back().buf == buf) return &t_sj.back();  // again (loop)
  SjEntry& e = t_sj.emplace_back();
  e.buf = buf;
  e.scope = &scope;
  return &e;
}

void sj_resume(Ctx& ctx) {
  ctx.r[3] = t_pending_buf;
  ctx.r[4] = t_pending_val;
  g_longjmp_code.load()(ctx);  // restores r1, r2, lr, cr, r14-r31, f14-f31 from the buffer; r3 = value
}

// hooks.txt "longjmp": the guest longjmp(buf, val) returns to the live setjmp call for buf.
void hook_longjmp(Ctx& ctx, Fn orig) {
  g_longjmp_code.store(orig);
  u32 buf = u32(ctx.r[3]);
  static std::atomic<u32> logged{0};
  if (logged.fetch_add(1) < 20)
    LOG_I("ppu", "guest longjmp to 0x%08x (value %d) from 0x%08x", buf, int(s32(ctx.r[4])), u32(ctx.lr));
  for (size_t i = t_sj.size(); i-- > 0;) {
    if (t_sj[i].buf != buf) continue;
    t_pending_buf = buf;
    t_pending_val = ctx.r[4];
    longjmp(t_sj[i].jb, 1);
  }
  trap(ctx, u32(ctx.lr), "guest longjmp to a buffer without a live setjmp call on this thread");
}

}  // namespace ppu
