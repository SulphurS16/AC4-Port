// Instruction semantics for generated (recompiled) PPU code and the interpreter.
// Everything here is header-only so clang can inline it into generated functions.
#pragma once
#include <immintrin.h>

#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>

#include "ppu/ppu_context.h"
#include "ppu/ppu_insn.h"

// Fixed host address of guest address 0 (4 GiB reserved by the runtime at startup).
#define PPU_MEM_BASE 0x0000200000000000ull

namespace ppu {

using Ctx = Context;
using Fn = void (*)(Context&);

// ---- runtime hooks (implemented by the runtime) ----
void icall(Ctx& ctx, u32 target);              // call guest code address (indirect)
void syscall(Ctx& ctx);                        // `sc`, number in r11
[[noreturn]] void trap(Ctx& ctx, u32 pc, const char* why);
void bad_switch(Ctx& ctx, u32 pc, u32 target);
u64 timebase();
u64 read_spr(Ctx& ctx, u32 pc, u32 spr);       // uncommon SPRs
void write_spr(Ctx& ctx, u32 pc, u32 spr, u64 v);
u32 lwarx32(Ctx& ctx, u32 ea);                  // reservation load (see runtime)
u64 ldarx64(Ctx& ctx, u32 ea);
bool stcx32(Ctx& ctx, u32 ea, u32 v);           // reservation store
bool stcx64(Ctx& ctx, u32 ea, u64 v);

inline u8* mem() { return reinterpret_cast<u8*>(PPU_MEM_BASE); }

// ---- memory (guest big-endian) ----
inline u8 r8(u32 a) { return mem()[a]; }
inline u16 r16(u32 a) { u16 v; std::memcpy(&v, mem() + a, 2); return __builtin_bswap16(v); }
inline u32 r32(u32 a) { u32 v; std::memcpy(&v, mem() + a, 4); return __builtin_bswap32(v); }
inline u64 r64(u32 a) { u64 v; std::memcpy(&v, mem() + a, 8); return __builtin_bswap64(v); }
inline void w8(u32 a, u8 v) { mem()[a] = v; }
inline void w16(u32 a, u16 v) { v = __builtin_bswap16(v); std::memcpy(mem() + a, &v, 2); }
inline void w32(u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(mem() + a, &v, 4); }
inline void w64(u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(mem() + a, &v, 8); }
inline u16 r16le(u32 a) { u16 v; std::memcpy(&v, mem() + a, 2); return v; }
inline u32 r32le(u32 a) { u32 v; std::memcpy(&v, mem() + a, 4); return v; }
inline u64 r64le(u32 a) { u64 v; std::memcpy(&v, mem() + a, 8); return v; }
inline void w16le(u32 a, u16 v) { std::memcpy(mem() + a, &v, 2); }
inline void w32le(u32 a, u32 v) { std::memcpy(mem() + a, &v, 4); }
inline void w64le(u32 a, u64 v) { std::memcpy(mem() + a, &v, 8); }

inline u64 dbits(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }
inline double bitsd(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
inline u32 fbits(float f) { u32 b; std::memcpy(&b, &f, 4); return b; }
inline float bitsf(u32 b) { float f; std::memcpy(&f, &b, 4); return f; }

inline double rf32(u32 a) { return double(bitsf(r32(a))); }
inline double rf64(u32 a) { return bitsd(r64(a)); }
inline void wf32(u32 a, double d) { w32(a, fbits(float(d))); }
inline void wf64(u32 a, double d) { w64(a, dbits(d)); }

// ---- condition register ----
template <typename T>
inline void crset(Ctx& c, int f, T a, T b) {
  c.cr[4 * f + 0] = a < b;
  c.cr[4 * f + 1] = a > b;
  c.cr[4 * f + 2] = a == b;
  c.cr[4 * f + 3] = c.so;
}
inline void cr0(Ctx& c, u64 v) { crset<s64>(c, 0, s64(v), 0); }
inline u32 mfcr(const Ctx& c) {
  u32 v = 0;
  for (int i = 0; i < 32; ++i) v |= u32(c.cr[i] & 1) << (31 - i);
  return v;
}
inline void mtcrf(Ctx& c, u32 crm, u64 s) {
  for (int f = 0; f < 8; ++f)
    if (crm & (0x80 >> f))
      for (int b = 0; b < 4; ++b) c.cr[4 * f + b] = (s >> (31 - (4 * f + b))) & 1;
}
inline u64 mfxer(const Ctx& c) { return (u64(c.so) << 31) | (u64(c.ov) << 30) | (u64(c.ca) << 29) | c.xer_cnt; }
inline void mtxer(Ctx& c, u64 v) {
  c.so = (v >> 31) & 1;
  c.ov = (v >> 30) & 1;
  c.ca = (v >> 29) & 1;
  c.xer_cnt = v & 0x7f;
}

// ---- integer helpers ----
inline u64 rotl64(u64 x, u32 n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
inline u64 rotl32d(u64 x, u32 n) {
  u32 v = u32(x);
  n &= 31;
  u32 r = n ? (v << n) | (v >> (32 - n)) : v;
  return (u64(r) << 32) | r;
}
inline u64 add_c(u64 a, u64 b, u8 cin, u8& cout) {
  unsigned __int128 s = (unsigned __int128)a + b + cin;
  cout = u8(s >> 64);
  return u64(s);
}
inline u8 ovf_add(u64 a, u64 b, u64 r) { return u8((((a ^ r) & (b ^ r)) >> 63) & 1); }
inline void set_ov(Ctx& c, u8 ov) {
  c.ov = ov;
  c.so |= ov;
}
inline u64 slw(u64 s, u64 b) { u32 n = b & 63; return (n & 32) ? 0 : u64(u32(u32(s) << n)); }
inline u64 srw(u64 s, u64 b) { u32 n = b & 63; return (n & 32) ? 0 : u64(u32(s) >> n); }
inline u64 sld(u64 s, u64 b) { u32 n = b & 127; return (n & 64) ? 0 : s << n; }
inline u64 srd(u64 s, u64 b) { u32 n = b & 127; return (n & 64) ? 0 : s >> n; }
inline u64 sraw(Ctx& c, u64 s, u32 n) {
  s32 v = s32(s);
  if (n & 32) {
    c.ca = v < 0;
    return u64(s64(v >> 31));
  }
  c.ca = (v < 0) && n && (u32(v) & ((1u << n) - 1));
  return u64(s64(v >> n));
}
inline u64 srad(Ctx& c, u64 s, u32 n) {
  s64 v = s64(s);
  if (n & 64) {
    c.ca = v < 0;
    return u64(v >> 63);
  }
  c.ca = (v < 0) && n && (u64(v) & ((1ull << n) - 1));
  return u64(v >> n);
}
inline u64 cntlzw(u64 x) { return u32(x) ? __builtin_clz(u32(x)) : 32; }
inline u64 cntlzd(u64 x) { return x ? __builtin_clzll(x) : 64; }
inline u64 divw(u64 a, u64 b) {
  s32 x = s32(a), y = s32(b);
  if (y == 0 || (x == INT32_MIN && y == -1)) return 0;
  return u64(u32(x / y));
}
inline u64 divwu(u64 a, u64 b) { return u32(b) ? u64(u32(a) / u32(b)) : 0; }
inline u64 divd(u64 a, u64 b) {
  s64 x = s64(a), y = s64(b);
  if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
  return u64(x / y);
}
inline u64 divdu(u64 a, u64 b) { return b ? a / b : 0; }
inline u64 mulhw(u64 a, u64 b) { return u64(s64((s64(s32(a)) * s32(b)) >> 32)); }
inline u64 mulhwu(u64 a, u64 b) { return (u64(u32(a)) * u32(b)) >> 32; }
inline u64 mulhd(u64 a, u64 b) { return u64((__int128(s64(a)) * s64(b)) >> 64); }
inline u64 mulhdu(u64 a, u64 b) { return u64(((unsigned __int128)a * b) >> 64); }
inline bool trap_if(u32 to, s64 a, s64 b) {
  return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) || ((to & 2) && u64(a) < u64(b)) ||
         ((to & 1) && u64(a) > u64(b));
}

// ---- load/store string & multiple ----
inline void lswi(Ctx& c, u32 ea, u32 rt, u32 nb) {
  u32 n = nb ? nb : 32, r = rt - 1, i = 0;
  while (n--) {
    if (i == 0) {
      r = (r + 1) & 31;
      c.r[r] = 0;
    }
    c.r[r] |= u64(r8(ea++)) << (24 - i);
    i = (i + 8) & 31;
  }
}
inline void stswi(Ctx& c, u32 ea, u32 rs, u32 nb) {
  u32 n = nb ? nb : 32, r = rs - 1, i = 0;
  while (n--) {
    if (i == 0) r = (r + 1) & 31;
    w8(ea++, u8(c.r[r] >> (24 - i)));
    i = (i + 8) & 31;
  }
}

// ---- FPU ----
inline double rs(double x) { return double(float(x)); }
inline double fsel(double a, double b, double c) { return a >= 0.0 ? c : b; }
inline double fctiw(double b, bool trunc) {
  s32 r;
  if (std::isnan(b)) r = INT32_MIN;
  else {
    double t = trunc ? std::trunc(b) : std::nearbyint(b);
    r = t >= 2147483648.0 ? INT32_MAX : t < -2147483648.0 ? INT32_MIN : s32(t);
  }
  return bitsd(u64(s64(r)));
}
inline double fctid(double b, bool trunc) {
  s64 r;
  if (std::isnan(b)) r = INT64_MIN;
  else {
    double t = trunc ? std::trunc(b) : std::nearbyint(b);
    r = t >= 9223372036854775808.0 ? INT64_MAX : t < -9223372036854775808.0 ? INT64_MIN : s64(t);
  }
  return bitsd(u64(r));
}
inline double fcfid(double b) { return double(s64(dbits(b))); }
inline void fcmp(Ctx& c, int f, double a, double b) {
  bool un = std::isnan(a) || std::isnan(b);
  u8 lt = !un && a < b, gt = !un && a > b, eq = !un && a == b;
  c.cr[4 * f + 0] = lt;
  c.cr[4 * f + 1] = gt;
  c.cr[4 * f + 2] = eq;
  c.cr[4 * f + 3] = un;
  c.fpscr = (c.fpscr & ~0xF000u) | (u32(lt) << 15) | (u32(gt) << 14) | (u32(eq) << 13) | (u32(un) << 12);
}
inline void cr1_fp(Ctx& c) {
  c.cr[4] = (c.fpscr >> 31) & 1;
  c.cr[5] = (c.fpscr >> 30) & 1;
  c.cr[6] = (c.fpscr >> 29) & 1;
  c.cr[7] = (c.fpscr >> 28) & 1;
}
inline void mtfsf(Ctx& c, u32 fm, double b) {
  u32 v = u32(dbits(b)), m = 0;
  for (int i = 0; i < 8; ++i)
    if (fm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i);
  c.fpscr = (c.fpscr & ~m) | (v & m);
}

// ---- VMX ----
inline __m128i vrev_mask() { return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
inline v128 lvx(u32 ea) {
  v128 r;
  r.vi = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(mem() + (ea & ~15u))), vrev_mask());
  return r;
}
inline void stvx(u32 ea, const v128& v) {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(mem() + (ea & ~15u)), _mm_shuffle_epi8(v.vi, vrev_mask()));
}
inline void stvebx(u32 ea, const v128& v) { w8(ea, v.u8x[15 - (ea & 15)]); }
inline void stvehx(u32 ea, const v128& v) { ea &= ~1u; w16(ea, v.u16x[7 - ((ea & 15) >> 1)]); }
inline void stvewx(u32 ea, const v128& v) { ea &= ~3u; w32(ea, v.u32x[3 - ((ea & 15) >> 2)]); }
inline v128 lvsl(u32 ea) {
  v128 r;
  u32 sh = ea & 15;
  for (int i = 0; i < 16; ++i) be8(r, i) = u8(sh + i);
  return r;
}
inline v128 lvsr(u32 ea) {
  v128 r;
  u32 sh = ea & 15;
  for (int i = 0; i < 16; ++i) be8(r, i) = u8(16 - sh + i);
  return r;
}
// Cell unaligned helpers: lvlx loads [ea, end of 16-byte block) left-justified; lvrx loads
// [block start, ea) right-justified.
inline v128 lvlx(u32 ea) {
  v128 r{};
  u32 sh = ea & 15;
  for (u32 i = 0; i < 16 - sh; ++i) be8(r, int(i)) = r8(ea + i);
  return r;
}
inline v128 lvrx(u32 ea) {
  v128 r{};
  u32 sh = ea & 15, base = ea & ~15u;
  for (u32 i = 0; i < sh; ++i) be8(r, int(16 - sh + i)) = r8(base + i);
  return r;
}
inline void stvlx(u32 ea, v128 v) {
  u32 sh = ea & 15;
  for (u32 i = 0; i < 16 - sh; ++i) w8(ea + i, be8(v, int(i)));
}
inline void stvrx(u32 ea, v128 v) {
  u32 sh = ea & 15, base = ea & ~15u;
  for (u32 i = 0; i < sh; ++i) w8(base + i, be8(v, int(16 - sh + i)));
}

inline v128 mk(__m128i x) { v128 r; r.vi = x; return r; }
inline v128 mkf(__m128 x) { v128 r; r.vf = x; return r; }

inline v128 vperm(const v128& a, const v128& b, const v128& c) {
  __m128i idx = _mm_andnot_si128(c.vi, _mm_set1_epi8(0x0f));  // host index = 15 - (sel & 15)
  __m128i ra = _mm_shuffle_epi8(a.vi, idx), rb = _mm_shuffle_epi8(b.vi, idx);
  __m128i selb = _mm_cmpeq_epi8(_mm_and_si128(c.vi, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
  return mk(_mm_blendv_epi8(ra, rb, selb));
}
inline v128 vsldoi(const v128& a, const v128& b, u32 sh) {
  v128 r;
  u8 x[32];
  std::memcpy(x, b.u8x, 16);
  std::memcpy(x + 16, a.u8x, 16);
  std::memcpy(r.u8x, x + 16 - sh, 16);
  return r;
}
inline v128 vsel(const v128& a, const v128& b, const v128& c) {
  return mk(_mm_or_si128(_mm_andnot_si128(c.vi, a.vi), _mm_and_si128(c.vi, b.vi)));
}
// Whole-register shifts: the host LE 128-bit integer equals the guest BE integer.
inline unsigned __int128 u128(const v128& v) { unsigned __int128 x; std::memcpy(&x, v.u8x, 16); return x; }
inline v128 from128(unsigned __int128 x) { v128 r; std::memcpy(r.u8x, &x, 16); return r; }
inline v128 vsl(const v128& a, const v128& b) { return from128(u128(a) << (b.u8x[0] & 7)); }
inline v128 vsr(const v128& a, const v128& b) { return from128(u128(a) >> (b.u8x[0] & 7)); }
inline v128 vslo(const v128& a, const v128& b) {
  u32 n = (b.u8x[0] >> 3) & 15;
  return n ? from128(u128(a) << (8 * n)) : a;
}
inline v128 vsro(const v128& a, const v128& b) {
  u32 n = (b.u8x[0] >> 3) & 15;
  return n ? from128(u128(a) >> (8 * n)) : a;
}
inline v128 vspltb(const v128& b, u32 i) { return mk(_mm_set1_epi8(s8(b.u8x[15 - (i & 15)]))); }
inline v128 vsplth(const v128& b, u32 i) { return mk(_mm_set1_epi16(s16(b.u16x[7 - (i & 7)]))); }
inline v128 vspltw(const v128& b, u32 i) { return mk(_mm_set1_epi32(s32(b.u32x[3 - (i & 3)]))); }

// Packs: guest result = [a elements..., b elements...] -> host low half from b, high half from a.
inline v128 vpkuhum(const v128& a, const v128& b) {
  __m128i m = _mm_set1_epi16(0xff);
  return mk(_mm_packus_epi16(_mm_and_si128(b.vi, m), _mm_and_si128(a.vi, m)));
}
inline v128 vpkuwum(const v128& a, const v128& b) {
  __m128i m = _mm_set1_epi32(0xffff);
  return mk(_mm_packus_epi32(_mm_and_si128(b.vi, m), _mm_and_si128(a.vi, m)));
}
inline v128 vpkuhus(const v128& a, const v128& b) {
  __m128i m = _mm_set1_epi16(0xff);
  return mk(_mm_packus_epi16(_mm_min_epu16(b.vi, m), _mm_min_epu16(a.vi, m)));
}
inline v128 vpkuwus(const v128& a, const v128& b) {
  __m128i m = _mm_set1_epi32(0xffff);
  return mk(_mm_packus_epi32(_mm_min_epu32(b.vi, m), _mm_min_epu32(a.vi, m)));
}
inline v128 vpkshss(const v128& a, const v128& b) { return mk(_mm_packs_epi16(b.vi, a.vi)); }
inline v128 vpkshus(const v128& a, const v128& b) { return mk(_mm_packus_epi16(b.vi, a.vi)); }
inline v128 vpkswss(const v128& a, const v128& b) { return mk(_mm_packs_epi32(b.vi, a.vi)); }
inline v128 vpkswus(const v128& a, const v128& b) { return mk(_mm_packus_epi32(b.vi, a.vi)); }
inline v128 vpkpx(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 8; ++i) {
    u32 w = i < 4 ? be32(const_cast<v128&>(a), i) : be32(const_cast<v128&>(b), i - 4);
    be16(r, i) = u16(((w >> 9) & 0x8000) | ((w >> 9) & 0x7c00) | ((w >> 6) & 0x3e0) | ((w >> 3) & 0x1f));
  }
  return r;
}
// Unpacks: high = guest elements 0..n/2-1 = host upper half.
inline v128 vupkhsb(const v128& b) { return mk(_mm_cvtepi8_epi16(_mm_srli_si128(b.vi, 8))); }
inline v128 vupklsb(const v128& b) { return mk(_mm_cvtepi8_epi16(b.vi)); }
inline v128 vupkhsh(const v128& b) { return mk(_mm_cvtepi16_epi32(_mm_srli_si128(b.vi, 8))); }
inline v128 vupklsh(const v128& b) { return mk(_mm_cvtepi16_epi32(b.vi)); }
inline v128 vupkpx(const v128& b, bool high) {
  v128 r;
  v128 bb = b;
  for (int i = 0; i < 4; ++i) {
    u16 p = be16(bb, high ? i : i + 4);
    u32 a = (p & 0x8000) ? 0xff : 0;
    be32(r, i) = (a << 24) | (u32((p >> 10) & 31) << 16) | (u32((p >> 5) & 31) << 8) | (p & 31);
  }
  return r;
}
// Merges.
inline v128 vmrghb(const v128& a, const v128& b) { return mk(_mm_unpackhi_epi8(b.vi, a.vi)); }
inline v128 vmrghh(const v128& a, const v128& b) { return mk(_mm_unpackhi_epi16(b.vi, a.vi)); }
inline v128 vmrghw(const v128& a, const v128& b) { return mk(_mm_unpackhi_epi32(b.vi, a.vi)); }
inline v128 vmrglb(const v128& a, const v128& b) { return mk(_mm_unpacklo_epi8(b.vi, a.vi)); }
inline v128 vmrglh(const v128& a, const v128& b) { return mk(_mm_unpacklo_epi16(b.vi, a.vi)); }
inline v128 vmrglw(const v128& a, const v128& b) { return mk(_mm_unpacklo_epi32(b.vi, a.vi)); }

// Element-wise integer arithmetic.
inline v128 vadduws(const v128& a, const v128& b) {
  __m128i s = _mm_add_epi32(a.vi, b.vi);
  __m128i ovf = _mm_cmpgt_epi32(_mm_xor_si128(a.vi, _mm_set1_epi32(INT32_MIN)), _mm_xor_si128(s, _mm_set1_epi32(INT32_MIN)));
  return mk(_mm_or_si128(s, ovf));
}
inline v128 vaddsws(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    s64 s = s64(a.s32x[i]) + b.s32x[i];
    r.s32x[i] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s32(s);
  }
  return r;
}
inline v128 vsubuws(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.u32x[i] = a.u32x[i] > b.u32x[i] ? a.u32x[i] - b.u32x[i] : 0;
  return r;
}
inline v128 vsubsws(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    s64 s = s64(a.s32x[i]) - b.s32x[i];
    r.s32x[i] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s32(s);
  }
  return r;
}
inline v128 vaddcuw(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.u32x[i] = u32(a.u32x[i] + b.u32x[i]) < a.u32x[i];
  return r;
}
inline v128 vsubcuw(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.u32x[i] = a.u32x[i] >= b.u32x[i];
  return r;
}
inline v128 vavguw(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.u32x[i] = u32((u64(a.u32x[i]) + b.u32x[i] + 1) >> 1);
  return r;
}
inline v128 vavgsb(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 16; ++i) r.s8x[i] = s8((s32(a.s8x[i]) + b.s8x[i] + 1) >> 1);
  return r;
}
inline v128 vavgsh(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 8; ++i) r.s16x[i] = s16((s32(a.s16x[i]) + b.s16x[i] + 1) >> 1);
  return r;
}
inline v128 vavgsw(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.s32x[i] = s32((s64(a.s32x[i]) + b.s32x[i] + 1) >> 1);
  return r;
}
template <typename T, int N>
inline v128 vshift(const v128& a, const v128& b, int kind) {  // 0 = sl, 1 = sr, 2 = sra, 3 = rl
  v128 r;
  const T* x = reinterpret_cast<const T*>(a.u8x);
  const T* s = reinterpret_cast<const T*>(b.u8x);
  T* o = reinterpret_cast<T*>(r.u8x);
  constexpr u32 bits = sizeof(T) * 8;
  for (int i = 0; i < N; ++i) {
    u32 n = u32(s[i]) & (bits - 1);
    using S = std::make_signed_t<T>;
    switch (kind) {
      case 0: o[i] = T(x[i] << n); break;
      case 1: o[i] = T(x[i] >> n); break;
      case 2: o[i] = T(S(x[i]) >> n); break;
      default: o[i] = n ? T((x[i] << n) | (x[i] >> (bits - n))) : x[i]; break;
    }
  }
  return r;
}
inline v128 vslw(const v128& a, const v128& b) { return mk(_mm_sllv_epi32(a.vi, _mm_and_si128(b.vi, _mm_set1_epi32(31)))); }
inline v128 vsrw(const v128& a, const v128& b) { return mk(_mm_srlv_epi32(a.vi, _mm_and_si128(b.vi, _mm_set1_epi32(31)))); }
inline v128 vsraw(const v128& a, const v128& b) { return mk(_mm_srav_epi32(a.vi, _mm_and_si128(b.vi, _mm_set1_epi32(31)))); }
inline v128 vrlw(const v128& a, const v128& b) {
  __m128i n = _mm_and_si128(b.vi, _mm_set1_epi32(31));
  return mk(_mm_or_si128(_mm_sllv_epi32(a.vi, n), _mm_srlv_epi32(a.vi, _mm_sub_epi32(_mm_set1_epi32(32), n))));
}
// Even/odd multiplies (guest even elements are the host odd/high parts of each wider lane).
inline v128 vmuleub(const v128& a, const v128& b) { return mk(_mm_mullo_epi16(_mm_srli_epi16(a.vi, 8), _mm_srli_epi16(b.vi, 8))); }
inline v128 vmuloub(const v128& a, const v128& b) {
  __m128i m = _mm_set1_epi16(0xff);
  return mk(_mm_mullo_epi16(_mm_and_si128(a.vi, m), _mm_and_si128(b.vi, m)));
}
inline v128 vmulesb(const v128& a, const v128& b) { return mk(_mm_mullo_epi16(_mm_srai_epi16(a.vi, 8), _mm_srai_epi16(b.vi, 8))); }
inline v128 vmulosb(const v128& a, const v128& b) {
  return mk(_mm_mullo_epi16(_mm_srai_epi16(_mm_slli_epi16(a.vi, 8), 8), _mm_srai_epi16(_mm_slli_epi16(b.vi, 8), 8)));
}
inline v128 vmuleuh(const v128& a, const v128& b) { return mk(_mm_mullo_epi32(_mm_srli_epi32(a.vi, 16), _mm_srli_epi32(b.vi, 16))); }
inline v128 vmulouh(const v128& a, const v128& b) {
  __m128i m = _mm_set1_epi32(0xffff);
  return mk(_mm_mullo_epi32(_mm_and_si128(a.vi, m), _mm_and_si128(b.vi, m)));
}
inline v128 vmulesh(const v128& a, const v128& b) { return mk(_mm_mullo_epi32(_mm_srai_epi32(a.vi, 16), _mm_srai_epi32(b.vi, 16))); }
inline v128 vmulosh(const v128& a, const v128& b) {
  return mk(_mm_mullo_epi32(_mm_srai_epi32(_mm_slli_epi32(a.vi, 16), 16), _mm_srai_epi32(_mm_slli_epi32(b.vi, 16), 16)));
}
// Multiply-sum family (scalar, lane-order independent within each word).
inline s32 sat32(s64 v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : s32(v); }
inline u32 satu32(s64 v) { return v > s64(UINT32_MAX) ? UINT32_MAX : v < 0 ? 0 : u32(v); }
inline v128 vmsumubm(const v128& a, const v128& b, const v128& c) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    u32 s = c.u32x[i];
    for (int j = 0; j < 4; ++j) s += u32(a.u8x[4 * i + j]) * b.u8x[4 * i + j];
    r.u32x[i] = s;
  }
  return r;
}
inline v128 vmsummbm(const v128& a, const v128& b, const v128& c) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    s32 s = c.s32x[i];
    for (int j = 0; j < 4; ++j) s += s32(a.s8x[4 * i + j]) * s32(b.u8x[4 * i + j]);
    r.s32x[i] = s;
  }
  return r;
}
inline v128 vmsumuhm(const v128& a, const v128& b, const v128& c) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.u32x[i] = c.u32x[i] + u32(a.u16x[2 * i]) * b.u16x[2 * i] + u32(a.u16x[2 * i + 1]) * b.u16x[2 * i + 1];
  return r;
}
inline v128 vmsumuhs(const v128& a, const v128& b, const v128& c) {
  v128 r;
  for (int i = 0; i < 4; ++i)
    r.u32x[i] = satu32(s64(c.u32x[i]) + s64(u32(a.u16x[2 * i]) * b.u16x[2 * i]) + s64(u32(a.u16x[2 * i + 1]) * b.u16x[2 * i + 1]));
  return r;
}
inline v128 vmsumshm(const v128& a, const v128& b, const v128& c) { return mk(_mm_add_epi32(_mm_madd_epi16(a.vi, b.vi), c.vi)); }
inline v128 vmsumshs(const v128& a, const v128& b, const v128& c) {
  v128 r;
  for (int i = 0; i < 4; ++i)
    r.s32x[i] = sat32(s64(c.s32x[i]) + s32(a.s16x[2 * i]) * b.s16x[2 * i] + s32(a.s16x[2 * i + 1]) * b.s16x[2 * i + 1]);
  return r;
}
inline v128 vmhaddshs(const v128& a, const v128& b, const v128& c, bool round) {
  v128 r;
  for (int i = 0; i < 8; ++i) {
    s32 p = s32(a.s16x[i]) * b.s16x[i] + (round ? 0x4000 : 0);
    s32 v = (p >> 15) + c.s16x[i];
    r.s16x[i] = s16(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
  }
  return r;
}
inline v128 vmladduhm(const v128& a, const v128& b, const v128& c) {
  return mk(_mm_add_epi16(_mm_mullo_epi16(a.vi, b.vi), c.vi));
}
inline v128 vsum4ubs(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    s64 s = b.u32x[i];
    for (int j = 0; j < 4; ++j) s += a.u8x[4 * i + j];
    r.u32x[i] = satu32(s);
  }
  return r;
}
inline v128 vsum4sbs(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    s64 s = b.s32x[i];
    for (int j = 0; j < 4; ++j) s += a.s8x[4 * i + j];
    r.s32x[i] = sat32(s);
  }
  return r;
}
inline v128 vsum4shs(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.s32x[i] = sat32(s64(b.s32x[i]) + a.s16x[2 * i] + a.s16x[2 * i + 1]);
  return r;
}
inline v128 vsum2sws(const v128& a, const v128& b) {
  v128 r{};
  v128 aa = a, bb = b;
  for (int i = 0; i < 2; ++i)
    be32(r, 2 * i + 1) = u32(sat32(s64(s32(be32(aa, 2 * i))) + s32(be32(aa, 2 * i + 1)) + s32(be32(bb, 2 * i + 1))));
  return r;
}
inline v128 vsumsws(const v128& a, const v128& b) {
  v128 r{};
  s64 s = s64(a.s32x[0]) + a.s32x[1] + a.s32x[2] + a.s32x[3] + s64(b.s32x[0]);  // guest b[3] = host b[0]
  r.s32x[0] = sat32(s);
  return r;
}

// Floating point.
inline v128 vrefp(const v128& b) { return mkf(_mm_div_ps(_mm_set1_ps(1.0f), b.vf)); }
inline v128 vrsqrtefp(const v128& b) { return mkf(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(b.vf))); }
inline v128 vexptefp(const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.fx[i] = std::exp2(b.fx[i]);
  return r;
}
inline v128 vlogefp(const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) r.fx[i] = std::log2(b.fx[i]);
  return r;
}
inline v128 vcfsx(const v128& b, u32 uim) {
  return mkf(_mm_mul_ps(_mm_cvtepi32_ps(b.vi), _mm_set1_ps(std::ldexp(1.0f, -int(uim)))));
}
inline v128 vcfux(const v128& b, u32 uim) {
  v128 r;
  float sc = std::ldexp(1.0f, -int(uim));
  for (int i = 0; i < 4; ++i) r.fx[i] = float(double(b.u32x[i]) * sc);
  return r;
}
inline v128 vctsxs(const v128& b, u32 uim) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    double x = double(b.fx[i]) * std::ldexp(1.0, int(uim));
    r.s32x[i] = std::isnan(x) ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : s32(std::trunc(x));
  }
  return r;
}
inline v128 vctuxs(const v128& b, u32 uim) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    double x = double(b.fx[i]) * std::ldexp(1.0, int(uim));
    r.u32x[i] = std::isnan(x) || x <= 0 ? 0 : x >= 4294967295.0 ? UINT32_MAX : u32(std::trunc(x));
  }
  return r;
}
inline v128 vmaxfp(const v128& a, const v128& b) { return mkf(_mm_max_ps(a.vf, b.vf)); }
inline v128 vminfp(const v128& a, const v128& b) { return mkf(_mm_min_ps(a.vf, b.vf)); }
inline v128 vcmpbfp(const v128& a, const v128& b) {
  v128 r;
  for (int i = 0; i < 4; ++i) {
    float x = a.fx[i], y = b.fx[i];
    u32 v = 0;
    if (!(x <= y)) v |= 0x80000000u;
    if (!(x >= -y)) v |= 0x40000000u;
    r.u32x[i] = v;
  }
  return r;
}
// Record-form vector compare: CR6 = [all true, 0, none true, 0].
inline void cr6(Ctx& c, const v128& r) {
  bool all = r.u64x[0] == ~0ull && r.u64x[1] == ~0ull;
  bool none = r.u64x[0] == 0 && r.u64x[1] == 0;
  c.cr[24] = all;
  c.cr[25] = 0;
  c.cr[26] = none;
  c.cr[27] = 0;
}
inline void cr6_bounds(Ctx& c, const v128& r) {
  c.cr[24] = 0;
  c.cr[25] = 0;
  c.cr[26] = r.u64x[0] == 0 && r.u64x[1] == 0;
  c.cr[27] = 0;
}

}  // namespace ppu
