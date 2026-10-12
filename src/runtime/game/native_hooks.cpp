// Native versions of small, hot guest leaf functions (listed in hooks.txt; facts in
// docs/GAME_NOTES.md, "Mission load"). Each returns exactly what the recompiled original returns;
// the originals only write below their stack pointer (dead after they return) and volatile
// registers. With --verify-native the original runs as well, its result is the one used, and
// differing results are logged: the game then behaves as without these hooks.
#include <atomic>
#include <cstring>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/log.h"

namespace ppu {
namespace {

struct Stats {
  const char* name;
  std::atomic<u64> calls{0}, work{0}, mismatches{0};
};

// Counts a call; logs the totals at 2^16, 2^17, ... calls. (Plain loads and stores: statistics
// only, and an atomic add per call cost ~5% of a mission load.)
void note_call(Stats& s, u64 work) {
  u64 n = s.calls.load(std::memory_order_relaxed) + 1;
  u64 w = s.work.load(std::memory_order_relaxed) + work;
  s.calls.store(n, std::memory_order_relaxed);
  s.work.store(w, std::memory_order_relaxed);
  if (n >= 65536 && (n & (n - 1)) == 0)
    LOG_I("native", "%s: %llu calls, %llu %s%s", s.name, (unsigned long long)n, (unsigned long long)w,
          s.name[0] == 'p' ? "entries scanned" : "bytes compared",
          rt::cfg().verify_native ? ac4::strprintf(", %llu mismatches", (unsigned long long)s.mismatches.load()).c_str() : "");
}

// --verify-native: runs the original and compares its r3 with the native result; args() describes
// the call (inputs captured by value; the originals leave the memory they read unchanged).
template <typename Args>
bool verify(Stats& s, Ctx& ctx, void (*orig)(Ctx&), u64 native, Args args) {
  if (!rt::cfg().verify_native) return false;
  orig(ctx);
  if (ctx.r[3] != native && s.mismatches.fetch_add(1) < 50)
    LOG_W("native", "%s mismatch: original 0x%llx, native 0x%llx (%s)", s.name, (unsigned long long)ctx.r[3],
          (unsigned long long)native, args().c_str());
  return true;
}

Stats g_name_compare{"name_compare"}, g_pair_find{"pair_find"};

}  // namespace

// 0x4ddddc (r3, r4: names): case-insensitive compare. The original copies both strings to
// 256-byte stack buffers (strcpy 0xd7dd28), lowercases A-Z in them and returns strcmp of the
// copies (0xd7da18): at the first differing byte -1 or 1 (bytes compared unsigned), else 0; the
// difference also sets cr0. Compared in place here: the copies only matter for strings of 256
// bytes or more (they overflow the buffers), so a compare that reaches byte 255 runs the original.
void hook_name_compare(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 a = u32(ctx.r[3]), b = u32(ctx.r[4]);
  const u8* m = mem();
  auto lower = [](u8 c) { return u8(c - 65) <= 25 ? u8(c + 32) : c; };
  u64 r = 0;
  u32 i = 0;
  for (;; ++i) {
    if (i == 255) {
      orig(ctx);
      return;
    }
    u8 x = lower(m[u32(a + i)]), y = lower(m[u32(b + i)]);
    if (x != y) {
      r = x < y ? ~0ull : 1;
      break;
    }
    if (!x) break;
  }
  note_call(g_name_compare, i + 1);
  if (verify(g_name_compare, ctx, orig, r, [=] {
        return ac4::strprintf("'%.40s' vs '%.40s'", reinterpret_cast<const char*>(m + a), reinterpret_cast<const char*>(m + b));
      }))
    return;
  ctx.r[3] = r;
  if (r) cr0(ctx, r);
}

// 0x54a648 (r3 object, r4 key A, r5 key B; a key: id byte at +0, u16 at +2): looks the pair up
// in the object's table of 8-byte entries {u8 id0, u8 id1, u16 x, u16 y, pad} (begin and end
// words at +0x53680): an entry for (A, B) returns 2, else one for (B, A) returns 1, else 0; keys
// with the same id byte return 0 without a search. Its only caller is 0x4fb10c.
void hook_pair_find(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = u32(ctx.r[3]), ka = u32(ctx.r[4]), kb = u32(ctx.r[5]);
  u8 a0 = r8(ka), b0 = r8(kb);
  u64 r = 0, scanned = 0;
  if (a0 != b0) {
    u16 a16 = r16(ka + 2), b16 = r16(kb + 2);
    u32 begin = r32(obj + 0x53680), end = r32(obj + 0x53684);
    auto find = [&](u8 i0, u8 i1, u16 x, u16 y) {
      // The entry's first 6 bytes in memory order: i0 i1 x(hi lo) y(hi lo).
      u64 want = u64(i0) | u64(i1) << 8 | u64(x >> 8) << 16 | u64(x & 0xff) << 24 | u64(y >> 8) << 32 | u64(y & 0xff) << 40;
      for (u32 e = begin; e != end; e += 8) {
        ++scanned;
        u64 v;
        std::memcpy(&v, mem() + e, 8);
        if ((v & 0xffffffffffffull) == want) return true;
      }
      return false;
    };
    r = find(a0, b0, a16, b16) ? 2 : find(b0, a0, b16, a16) ? 1 : 0;
  }
  note_call(g_pair_find, scanned);
  if (verify(g_pair_find, ctx, orig, r,
             [=] { return ac4::strprintf("object 0x%08x, keys 0x%08x 0x%08x", obj, r32(ka), r32(kb)); }))
    return;
  ctx.r[3] = r;
}

}  // namespace ppu
