// SPU instruction semantics shared by the interpreter and recompiled code.
//
// Each non-control instruction is a macro SPU_OP_<NAME> that expects these names in scope:
//   t (rt::spu::Thread&), g (ppu::v128* registers), pc (u32), w (u32 instruction word),
//   rt, ra, rb, rrr_t, rrr_c (u32), i7, i10, s16 (s32), i16, i18 (u32).
// Registers use the PPU VMX host layout (byte-reversed): host u32x[3] is the preferred slot.
// Control transfers, channels and stop are handled by the interpreter/emitter directly.
#pragma once
#include <immintrin.h>

#include <cmath>
#include <cstring>

#include "runtime/spu/spu.h"

namespace spu::ops {

using ppu::v128;
using s16_t = int16_t;

inline u32 pref(const v128& r) { return r.u32x[3]; }
inline v128 splat32(u32 v) {
  v128 r;
  r.vi = _mm_set1_epi32(s32(v));
  return r;
}
inline v128 scalar(u32 v) {
  v128 r;
  r.vi = _mm_setr_epi32(0, 0, 0, s32(v));
  return r;
}
inline unsigned __int128 q(const v128& v) {
  unsigned __int128 x;
  std::memcpy(&x, v.u8x, 16);
  return x;
}
inline v128 fromq(unsigned __int128 x) {
  v128 r;
  std::memcpy(r.u8x, &x, 16);
  return r;
}
// Bit-granular quadword shifts (0..7 bits).
inline v128 rotl128_bits(const v128& v, u32 bits) {
  bits &= 7;
  if (!bits) return v;
  unsigned __int128 x = q(v);
  return fromq((x << bits) | (x >> (128 - bits)));
}
inline v128 shl128_bits(const v128& v, u32 bits) { return fromq(q(v) << (bits & 7)); }
inline v128 shr128_bits(const v128& v, u32 bits) { return fromq(q(v) >> (bits & 7)); }

// pshufb masks for byte-granular quadword shifts/rotates (host byte order).
struct ByteMasks {
  __m128i rot[16], shl[16], shr[16];
  ByteMasks() {
    for (int n = 0; n < 16; ++n) {
      alignas(16) u8 r[16], l[16], s[16];
      for (int j = 0; j < 16; ++j) {
        r[j] = u8((j - n) & 15);                     // BE rotate left by n bytes
        l[j] = j - n >= 0 ? u8(j - n) : u8(0x80);    // BE shift left by n bytes
        s[j] = j + n < 16 ? u8(j + n) : u8(0x80);    // BE shift right by n bytes
      }
      rot[n] = _mm_load_si128(reinterpret_cast<const __m128i*>(r));
      shl[n] = _mm_load_si128(reinterpret_cast<const __m128i*>(l));
      shr[n] = _mm_load_si128(reinterpret_cast<const __m128i*>(s));
    }
  }
};
inline const ByteMasks g_byte_masks;

inline v128 rotqby(const v128& v, u32 n) {
  v128 r;
  r.vi = _mm_shuffle_epi8(v.vi, g_byte_masks.rot[n & 15]);
  return r;
}
inline v128 shlqby(const v128& v, u32 n) {  // n in 0..31 bytes
  v128 r;
  r.vi = n >= 16 ? _mm_setzero_si128() : _mm_shuffle_epi8(v.vi, g_byte_masks.shl[n]);
  return r;
}
inline v128 shrqby(const v128& v, u32 n) {  // n in 0..31 bytes
  v128 r;
  r.vi = n >= 16 ? _mm_setzero_si128() : _mm_shuffle_epi8(v.vi, g_byte_masks.shr[n]);
  return r;
}

inline __m128i rev_mask() { return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }

inline v128 load_ls(rt::spu::Thread& t, u32 addr) {
  v128 r;
  r.vi = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t.ls + (addr & 0x3fff0))), rev_mask());
  return r;
}
inline void store_ls(rt::spu::Thread& t, u32 addr, const v128& v) {
  addr &= 0x3fff0;
  _mm_storeu_si128(reinterpret_cast<__m128i*>(t.ls + addr), _mm_shuffle_epi8(v.vi, rev_mask()));
  if ((t.code_lines[addr >> 13] >> ((addr >> 7) & 63)) & 1) ++t.code_gen;  // code was overwritten
}

// shufb: per BE result byte, control c selects a byte of a||b (c & 0x1F) or a constant
// (10xxxxxx -> 0x00, 110xxxxx -> 0xFF, 111xxxxx -> 0x80).
inline v128 shufb(const v128& a, const v128& b, const v128& c) {
  const __m128i cc = c.vi;
  const __m128i idx = _mm_and_si128(_mm_xor_si128(cc, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x0F));
  const __m128i va = _mm_shuffle_epi8(a.vi, idx), vb = _mm_shuffle_epi8(b.vi, idx);
  const __m128i from_b = _mm_cmpeq_epi8(_mm_and_si128(cc, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
  __m128i r = _mm_blendv_epi8(va, vb, from_b);
  const __m128i c0 = _mm_cmpeq_epi8(_mm_and_si128(cc, _mm_set1_epi8(char(0xC0))), _mm_set1_epi8(char(0xC0)));
  const __m128i e0 = _mm_cmpeq_epi8(_mm_and_si128(cc, _mm_set1_epi8(char(0xE0))), _mm_set1_epi8(char(0xE0)));
  const __m128i special = _mm_or_si128(_mm_andnot_si128(e0, c0), _mm_and_si128(e0, _mm_set1_epi8(char(0x80))));
  r = _mm_blendv_epi8(r, special, _mm_cmplt_epi8(cc, _mm_setzero_si128()));
  v128 out;
  out.vi = r;
  return out;
}

// Insertion controls for cbd/chd/cwd/cdd (BE byte offset, element size 1/2/4/8).
struct InsertTable {
  v128 v[4][16];
  InsertTable() {
    const u32 sizes[4] = {1, 2, 4, 8};
    for (int s = 0; s < 4; ++s)
      for (u32 off = 0; off < 16; ++off) {
        v128 r;
        for (int i = 0; i < 16; ++i) r.u8x[15 - i] = u8(0x10 + i);
        u32 n = sizes[s];
        if (off + n <= 16)
          for (u32 k = 0; k < n; ++k) r.u8x[15 - (off + k)] = u8(n == 1 ? 0x03 : n == 2 ? 0x02 + k : k);
        v[s][off] = r;
      }
  }
};
inline const InsertTable g_insert;
inline v128 insert_ctrl(u32 off, int size_index) { return g_insert.v[size_index][off & 15]; }

[[noreturn]] void halt(rt::spu::Thread& t, const char* why, u32 pc, u32 w);

template <typename F>
inline v128 map32(const v128& a, F f) {
  v128 r;
  for (int k = 0; k < 4; ++k) r.u32x[k] = f(a.u32x[k], k);
  return r;
}

}  // namespace spu::ops

// ---- per-instruction semantics ----
#define SPU_PREF(r) (::spu::ops::pref(g[r]))
#define SPU_SET(r, expr) (g[r] = (expr))

#define SPU_OP_SELB g[rrr_t].vi = _mm_or_si128(_mm_andnot_si128(g[rrr_c].vi, g[ra].vi), _mm_and_si128(g[rrr_c].vi, g[rb].vi))
#define SPU_OP_SHUFB g[rrr_t] = ::spu::ops::shufb(g[ra], g[rb], g[rrr_c])
#define SPU_OP_MPYA                                                                                           \
  {                                                                                                           \
    ppu::v128 r_;                                                                                             \
    for (int k = 0; k < 4; ++k)                                                                               \
      r_.s32x[k] = s32(int16_t(g[ra].u32x[k])) * s32(int16_t(g[rb].u32x[k])) + g[rrr_c].s32x[k];              \
    g[rrr_t] = r_;                                                                                            \
  }
#define SPU_OP_FMA g[rrr_t].vf = _mm_fmadd_ps(g[ra].vf, g[rb].vf, g[rrr_c].vf)
#define SPU_OP_FMS g[rrr_t].vf = _mm_fmsub_ps(g[ra].vf, g[rb].vf, g[rrr_c].vf)
#define SPU_OP_FNMS g[rrr_t].vf = _mm_fnmadd_ps(g[ra].vf, g[rb].vf, g[rrr_c].vf)

#define SPU_OP_ILA g[rt] = ::spu::ops::splat32(i18)
#define SPU_OP_IL g[rt] = ::spu::ops::splat32(u32(s16))
#define SPU_OP_ILH g[rt].vi = _mm_set1_epi16(int16_t(i16))
#define SPU_OP_ILHU g[rt] = ::spu::ops::splat32(i16 << 16)
#define SPU_OP_IOHL g[rt].vi = _mm_or_si128(g[rt].vi, _mm_set1_epi32(s32(i16)))
#define SPU_OP_FSMBI                                                                       \
  {                                                                                        \
    ppu::v128 r_;                                                                          \
    for (int i = 0; i < 16; ++i) r_.u8x[15 - i] = (i16 & (0x8000 >> i)) ? 0xFF : 0x00;     \
    g[rt] = r_;                                                                            \
  }
#define SPU_OP_HBRA
#define SPU_OP_HBRR
#define SPU_OP_HBR
#define SPU_OP_LNOP
#define SPU_OP_NOP
#define SPU_OP_SYNC std::atomic_thread_fence(std::memory_order_seq_cst)
#define SPU_OP_DSYNC std::atomic_thread_fence(std::memory_order_seq_cst)

#define SPU_OP_LQD g[rt] = ::spu::ops::load_ls(t, SPU_PREF(ra) + u32(i10 << 4))
#define SPU_OP_LQX g[rt] = ::spu::ops::load_ls(t, SPU_PREF(ra) + SPU_PREF(rb))
#define SPU_OP_LQA g[rt] = ::spu::ops::load_ls(t, u32(s16 << 2))
#define SPU_OP_LQR g[rt] = ::spu::ops::load_ls(t, pc + u32(s16 << 2))
#define SPU_OP_STQD ::spu::ops::store_ls(t, SPU_PREF(ra) + u32(i10 << 4), g[rt])
#define SPU_OP_STQX ::spu::ops::store_ls(t, SPU_PREF(ra) + SPU_PREF(rb), g[rt])
#define SPU_OP_STQA ::spu::ops::store_ls(t, u32(s16 << 2), g[rt])
#define SPU_OP_STQR ::spu::ops::store_ls(t, pc + u32(s16 << 2), g[rt])
#define SPU_OP_CBD g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + u32(i7)) & 0xF, 0)
#define SPU_OP_CHD g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + u32(i7)) & 0xE, 1)
#define SPU_OP_CWD g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + u32(i7)) & 0xC, 2)
#define SPU_OP_CDD g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + u32(i7)) & 0x8, 3)
#define SPU_OP_CBX g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + SPU_PREF(rb)) & 0xF, 0)
#define SPU_OP_CHX g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + SPU_PREF(rb)) & 0xE, 1)
#define SPU_OP_CWX g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + SPU_PREF(rb)) & 0xC, 2)
#define SPU_OP_CDX g[rt] = ::spu::ops::insert_ctrl((SPU_PREF(ra) + SPU_PREF(rb)) & 0x8, 3)

#define SPU_OP_A g[rt].vi = _mm_add_epi32(g[ra].vi, g[rb].vi)
#define SPU_OP_AH g[rt].vi = _mm_add_epi16(g[ra].vi, g[rb].vi)
#define SPU_OP_AI g[rt].vi = _mm_add_epi32(g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_AHI g[rt].vi = _mm_add_epi16(g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_SF g[rt].vi = _mm_sub_epi32(g[rb].vi, g[ra].vi)
#define SPU_OP_SFH g[rt].vi = _mm_sub_epi16(g[rb].vi, g[ra].vi)
#define SPU_OP_SFI g[rt].vi = _mm_sub_epi32(_mm_set1_epi32(i10), g[ra].vi)
#define SPU_OP_SFHI g[rt].vi = _mm_sub_epi16(_mm_set1_epi16(int16_t(i10)), g[ra].vi)
#define SPU_LOOP4(expr)                          \
  {                                              \
    ppu::v128 r_;                                \
    for (int k = 0; k < 4; ++k) r_.u32x[k] = (expr); \
    g[rt] = r_;                                  \
  }
#define SPU_LOOP4S(expr)                         \
  {                                              \
    ppu::v128 r_;                                \
    for (int k = 0; k < 4; ++k) r_.s32x[k] = (expr); \
    g[rt] = r_;                                  \
  }
#define SPU_OP_CG SPU_LOOP4(u32((u64(g[ra].u32x[k]) + g[rb].u32x[k]) >> 32))
#define SPU_OP_BG SPU_LOOP4(g[rb].u32x[k] >= g[ra].u32x[k] ? 1u : 0u)
#define SPU_OP_ADDX SPU_LOOP4(g[ra].u32x[k] + g[rb].u32x[k] + (g[rt].u32x[k] & 1))
#define SPU_OP_SFX SPU_LOOP4(g[rb].u32x[k] + ~g[ra].u32x[k] + (g[rt].u32x[k] & 1))
#define SPU_OP_CGX SPU_LOOP4(u32((u64(g[ra].u32x[k]) + g[rb].u32x[k] + (g[rt].u32x[k] & 1)) >> 32))
#define SPU_OP_BGX \
  SPU_LOOP4((s64(u64(g[rb].u32x[k])) - s64(u64(g[ra].u32x[k])) - s64(1 - (g[rt].u32x[k] & 1))) >= 0 ? 1u : 0u)
#define SPU_OP_MPY SPU_LOOP4S(s32(int16_t(g[ra].u32x[k])) * s32(int16_t(g[rb].u32x[k])))
#define SPU_OP_MPYU SPU_LOOP4(u32(u16(g[ra].u32x[k])) * u32(u16(g[rb].u32x[k])))
#define SPU_OP_MPYI SPU_LOOP4S(s32(int16_t(g[ra].u32x[k])) * i10)
#define SPU_OP_MPYUI SPU_LOOP4(u32(u16(g[ra].u32x[k])) * u32(u16(i10)))
#define SPU_OP_MPYH SPU_LOOP4((u32(u16(g[ra].u32x[k] >> 16)) * u32(u16(g[rb].u32x[k]))) << 16)
#define SPU_OP_MPYS SPU_LOOP4S((s32(int16_t(g[ra].u32x[k])) * s32(int16_t(g[rb].u32x[k]))) >> 16)
#define SPU_OP_MPYHH SPU_LOOP4S(s32(int16_t(g[ra].u32x[k] >> 16)) * s32(int16_t(g[rb].u32x[k] >> 16)))
#define SPU_OP_MPYHHU SPU_LOOP4(u32(u16(g[ra].u32x[k] >> 16)) * u32(u16(g[rb].u32x[k] >> 16)))
#define SPU_OP_MPYHHA \
  SPU_LOOP4S(g[rt].s32x[k] + s32(int16_t(g[ra].u32x[k] >> 16)) * s32(int16_t(g[rb].u32x[k] >> 16)))
#define SPU_OP_MPYHHAU \
  SPU_LOOP4(g[rt].u32x[k] + u32(u16(g[ra].u32x[k] >> 16)) * u32(u16(g[rb].u32x[k] >> 16)))
#define SPU_OP_CLZ SPU_LOOP4(g[ra].u32x[k] ? u32(__builtin_clz(g[ra].u32x[k])) : 32u)
#define SPU_OP_CNTB                                                                     \
  {                                                                                     \
    ppu::v128 r_;                                                                       \
    for (int k = 0; k < 16; ++k) r_.u8x[k] = u8(__builtin_popcount(g[ra].u8x[k]));       \
    g[rt] = r_;                                                                         \
  }
#define SPU_OP_FSMB                                                                      \
  {                                                                                      \
    u32 m_ = SPU_PREF(ra) & 0xffff;                                                      \
    ppu::v128 r_;                                                                        \
    for (int i = 0; i < 16; ++i) r_.u8x[15 - i] = (m_ & (0x8000 >> i)) ? 0xFF : 0;       \
    g[rt] = r_;                                                                          \
  }
#define SPU_OP_FSMH                                                                      \
  {                                                                                      \
    u32 m_ = SPU_PREF(ra) & 0xff;                                                        \
    ppu::v128 r_;                                                                        \
    for (int i = 0; i < 8; ++i) r_.u16x[7 - i] = (m_ & (0x80 >> i)) ? 0xFFFF : 0;        \
    g[rt] = r_;                                                                          \
  }
#define SPU_OP_FSM                                                                       \
  {                                                                                      \
    u32 m_ = SPU_PREF(ra) & 0xf;                                                         \
    ppu::v128 r_;                                                                        \
    for (int i = 0; i < 4; ++i) r_.u32x[3 - i] = (m_ & (0x8 >> i)) ? 0xFFFFFFFFu : 0;    \
    g[rt] = r_;                                                                          \
  }
#define SPU_OP_GBB                                                                        \
  {                                                                                       \
    /* LSB of host byte k (BE byte 15-k) -> bit k: BE byte 0 lands in bit 15 */           \
    u32 m_ = u32(_mm_movemask_epi8(_mm_slli_epi16(g[ra].vi, 7))) & 0xffff;                 \
    g[rt] = ::spu::ops::scalar(m_);                                                       \
  }
#define SPU_OP_GBH                                                                        \
  {                                                                                       \
    u32 m_ = 0;                                                                           \
    for (int i = 0; i < 8; ++i) m_ = (m_ << 1) | (g[ra].u16x[7 - i] & 1);                 \
    g[rt] = ::spu::ops::scalar(m_);                                                       \
  }
#define SPU_OP_GB                                                                         \
  {                                                                                       \
    u32 m_ = 0;                                                                           \
    for (int i = 0; i < 4; ++i) m_ = (m_ << 1) | (g[ra].u32x[3 - i] & 1);                 \
    g[rt] = ::spu::ops::scalar(m_);                                                       \
  }
#define SPU_OP_AVGB g[rt].vi = _mm_avg_epu8(g[ra].vi, g[rb].vi)
#define SPU_OP_ABSDB g[rt].vi = _mm_sub_epi8(_mm_max_epu8(g[ra].vi, g[rb].vi), _mm_min_epu8(g[ra].vi, g[rb].vi))
#define SPU_OP_SUMB                                                                        \
  {                                                                                        \
    ppu::v128 r_;                                                                          \
    for (int k = 0; k < 4; ++k) {                                                          \
      u32 sa_ = 0, sb_ = 0;                                                                \
      for (int j = 0; j < 4; ++j) {                                                        \
        sa_ += g[ra].u8x[4 * k + j];                                                       \
        sb_ += g[rb].u8x[4 * k + j];                                                       \
      }                                                                                    \
      r_.u32x[k] = (sb_ << 16) | (sa_ & 0xffff);                                           \
    }                                                                                      \
    g[rt] = r_;                                                                            \
  }
#define SPU_OP_XSBH                                                                       \
  {                                                                                       \
    ppu::v128 r_;                                                                         \
    for (int k = 0; k < 8; ++k) r_.s16x[k] = int16_t(s8(g[ra].u16x[k] & 0xff));           \
    g[rt] = r_;                                                                           \
  }
#define SPU_OP_XSHW SPU_LOOP4S(s32(int16_t(g[ra].u32x[k] & 0xffff)))
#define SPU_OP_XSWD                                                                       \
  {                                                                                       \
    ppu::v128 r_;                                                                         \
    for (int k = 0; k < 2; ++k) r_.u64x[k] = u64(s64(s32(u32(g[ra].u64x[k]))));           \
    g[rt] = r_;                                                                           \
  }

#define SPU_OP_AND g[rt].vi = _mm_and_si128(g[ra].vi, g[rb].vi)
#define SPU_OP_ANDC g[rt].vi = _mm_andnot_si128(g[rb].vi, g[ra].vi)
#define SPU_OP_OR g[rt].vi = _mm_or_si128(g[ra].vi, g[rb].vi)
#define SPU_OP_ORC g[rt].vi = _mm_or_si128(g[ra].vi, _mm_xor_si128(g[rb].vi, _mm_set1_epi32(-1)))
#define SPU_OP_XOR g[rt].vi = _mm_xor_si128(g[ra].vi, g[rb].vi)
#define SPU_OP_NAND g[rt].vi = _mm_xor_si128(_mm_and_si128(g[ra].vi, g[rb].vi), _mm_set1_epi32(-1))
#define SPU_OP_NOR g[rt].vi = _mm_xor_si128(_mm_or_si128(g[ra].vi, g[rb].vi), _mm_set1_epi32(-1))
#define SPU_OP_EQV g[rt].vi = _mm_xor_si128(_mm_xor_si128(g[ra].vi, g[rb].vi), _mm_set1_epi32(-1))
#define SPU_OP_ORX \
  g[rt] = ::spu::ops::scalar(g[ra].u32x[0] | g[ra].u32x[1] | g[ra].u32x[2] | g[ra].u32x[3])
#define SPU_OP_ANDI g[rt].vi = _mm_and_si128(g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_ANDHI g[rt].vi = _mm_and_si128(g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_ANDBI g[rt].vi = _mm_and_si128(g[ra].vi, _mm_set1_epi8(s8(i10)))
#define SPU_OP_ORI g[rt].vi = _mm_or_si128(g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_ORHI g[rt].vi = _mm_or_si128(g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_ORBI g[rt].vi = _mm_or_si128(g[ra].vi, _mm_set1_epi8(s8(i10)))
#define SPU_OP_XORI g[rt].vi = _mm_xor_si128(g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_XORHI g[rt].vi = _mm_xor_si128(g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_XORBI g[rt].vi = _mm_xor_si128(g[ra].vi, _mm_set1_epi8(s8(i10)))

// Element shifts/rotates: count from rb elements (register form) or i7 (immediate form).
#define SPU_SHL32(cnt) SPU_LOOP4(([&] { u32 n_ = (cnt) & 0x3f; return n_ > 31 ? 0u : g[ra].u32x[k] << n_; }()))
#define SPU_OP_SHL SPU_SHL32(g[rb].u32x[k])
#define SPU_OP_SHLI SPU_SHL32(u32(i7))
#define SPU_SHL16(cnt)                                                                  \
  {                                                                                     \
    ppu::v128 r_;                                                                       \
    for (int k = 0; k < 8; ++k) {                                                       \
      u32 n_ = (cnt) & 0x1f;                                                            \
      r_.u16x[k] = n_ > 15 ? 0 : u16(g[ra].u16x[k] << n_);                              \
    }                                                                                   \
    g[rt] = r_;                                                                         \
  }
#define SPU_OP_SHLH SPU_SHL16(u32(g[rb].u16x[k]))
#define SPU_OP_SHLHI SPU_SHL16(u32(i7))
#define SPU_ROT32(cnt) \
  SPU_LOOP4(([&] { u32 n_ = (cnt) & 0x1f, x_ = g[ra].u32x[k]; return n_ ? (x_ << n_) | (x_ >> (32 - n_)) : x_; }()))
#define SPU_OP_ROT SPU_ROT32(g[rb].u32x[k])
#define SPU_OP_ROTI SPU_ROT32(u32(i7))
#define SPU_ROT16(cnt)                                                                  \
  {                                                                                     \
    ppu::v128 r_;                                                                       \
    for (int k = 0; k < 8; ++k) {                                                       \
      u32 n_ = (cnt) & 0xf, x_ = g[ra].u16x[k];                                         \
      r_.u16x[k] = u16(n_ ? (x_ << n_) | (x_ >> (16 - n_)) : x_);                       \
    }                                                                                   \
    g[rt] = r_;                                                                         \
  }
#define SPU_OP_ROTH SPU_ROT16(u32(g[rb].u16x[k]))
#define SPU_OP_ROTHI SPU_ROT16(u32(i7))
#define SPU_ROTM32(cnt) SPU_LOOP4(([&] { u32 n_ = (0u - (cnt)) & 0x3f; return n_ > 31 ? 0u : g[ra].u32x[k] >> n_; }()))
#define SPU_OP_ROTM SPU_ROTM32(g[rb].u32x[k])
#define SPU_OP_ROTMI SPU_ROTM32(u32(i7))
#define SPU_ROTM16(cnt)                                                                 \
  {                                                                                     \
    ppu::v128 r_;                                                                       \
    for (int k = 0; k < 8; ++k) {                                                       \
      u32 n_ = (0u - (cnt)) & 0x1f;                                                     \
      r_.u16x[k] = n_ > 15 ? 0 : u16(g[ra].u16x[k] >> n_);                              \
    }                                                                                   \
    g[rt] = r_;                                                                         \
  }
#define SPU_OP_ROTHM SPU_ROTM16(u32(g[rb].u16x[k]))
#define SPU_OP_ROTHMI SPU_ROTM16(u32(i7))
#define SPU_ROTMA32(cnt) \
  SPU_LOOP4S(([&] { u32 n_ = (0u - (cnt)) & 0x3f; return g[ra].s32x[k] >> (n_ < 31 ? n_ : 31); }()))
#define SPU_OP_ROTMA SPU_ROTMA32(g[rb].u32x[k])
#define SPU_OP_ROTMAI SPU_ROTMA32(u32(i7))
#define SPU_ROTMA16(cnt)                                                                \
  {                                                                                     \
    ppu::v128 r_;                                                                       \
    for (int k = 0; k < 8; ++k) {                                                       \
      u32 n_ = (0u - (cnt)) & 0x1f;                                                     \
      r_.s16x[k] = int16_t(g[ra].s16x[k] >> (n_ < 15 ? n_ : 15));                       \
    }                                                                                   \
    g[rt] = r_;                                                                         \
  }
#define SPU_OP_ROTMAH SPU_ROTMA16(u32(g[rb].u16x[k]))
#define SPU_OP_ROTMAHI SPU_ROTMA16(u32(i7))

// Quadword shifts/rotates.
#define SPU_OP_ROTQBY g[rt] = ::spu::ops::rotqby(g[ra], SPU_PREF(rb) & 0xf)
#define SPU_OP_ROTQBYI g[rt] = ::spu::ops::rotqby(g[ra], u32(i7) & 0xf)
#define SPU_OP_ROTQBYBI g[rt] = ::spu::ops::rotqby(g[ra], (SPU_PREF(rb) >> 3) & 0xf)
#define SPU_OP_ROTQBI g[rt] = ::spu::ops::rotl128_bits(g[ra], SPU_PREF(rb) & 7)
#define SPU_OP_ROTQBII g[rt] = ::spu::ops::rotl128_bits(g[ra], u32(i7) & 7)
#define SPU_OP_SHLQBY g[rt] = ::spu::ops::shlqby(g[ra], SPU_PREF(rb) & 0x1f)
#define SPU_OP_SHLQBYI g[rt] = ::spu::ops::shlqby(g[ra], u32(i7) & 0x1f)
#define SPU_OP_SHLQBYBI g[rt] = ::spu::ops::shlqby(g[ra], (SPU_PREF(rb) >> 3) & 0x1f)
#define SPU_OP_SHLQBI g[rt] = ::spu::ops::shl128_bits(g[ra], SPU_PREF(rb) & 7)
#define SPU_OP_SHLQBII g[rt] = ::spu::ops::shl128_bits(g[ra], u32(i7) & 7)
#define SPU_OP_ROTQMBY g[rt] = ::spu::ops::shrqby(g[ra], (0u - SPU_PREF(rb)) & 0x1f)
#define SPU_OP_ROTQMBYI g[rt] = ::spu::ops::shrqby(g[ra], (0u - u32(i7)) & 0x1f)
#define SPU_OP_ROTQMBYBI g[rt] = ::spu::ops::shrqby(g[ra], (0u - (SPU_PREF(rb) >> 3)) & 0x1f)
#define SPU_OP_ROTQMBI g[rt] = ::spu::ops::shr128_bits(g[ra], (0u - SPU_PREF(rb)) & 7)
#define SPU_OP_ROTQMBII g[rt] = ::spu::ops::shr128_bits(g[ra], (0u - u32(i7)) & 7)

// Compares.
#define SPU_OP_CEQ g[rt].vi = _mm_cmpeq_epi32(g[ra].vi, g[rb].vi)
#define SPU_OP_CEQH g[rt].vi = _mm_cmpeq_epi16(g[ra].vi, g[rb].vi)
#define SPU_OP_CEQB g[rt].vi = _mm_cmpeq_epi8(g[ra].vi, g[rb].vi)
#define SPU_OP_CEQI g[rt].vi = _mm_cmpeq_epi32(g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_CEQHI g[rt].vi = _mm_cmpeq_epi16(g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_CEQBI g[rt].vi = _mm_cmpeq_epi8(g[ra].vi, _mm_set1_epi8(s8(i10)))
#define SPU_OP_CGT g[rt].vi = _mm_cmpgt_epi32(g[ra].vi, g[rb].vi)
#define SPU_OP_CGTH g[rt].vi = _mm_cmpgt_epi16(g[ra].vi, g[rb].vi)
#define SPU_OP_CGTB g[rt].vi = _mm_cmpgt_epi8(g[ra].vi, g[rb].vi)
#define SPU_OP_CGTI g[rt].vi = _mm_cmpgt_epi32(g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_CGTHI g[rt].vi = _mm_cmpgt_epi16(g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_CGTBI g[rt].vi = _mm_cmpgt_epi8(g[ra].vi, _mm_set1_epi8(s8(i10)))
#define SPU_CLGT(cmp, set1, x, y) g[rt].vi = cmp(_mm_xor_si128(x, set1), _mm_xor_si128(y, set1))
#define SPU_OP_CLGT SPU_CLGT(_mm_cmpgt_epi32, _mm_set1_epi32(INT32_MIN), g[ra].vi, g[rb].vi)
#define SPU_OP_CLGTH SPU_CLGT(_mm_cmpgt_epi16, _mm_set1_epi16(-32768), g[ra].vi, g[rb].vi)
#define SPU_OP_CLGTB SPU_CLGT(_mm_cmpgt_epi8, _mm_set1_epi8(-128), g[ra].vi, g[rb].vi)
#define SPU_OP_CLGTI SPU_CLGT(_mm_cmpgt_epi32, _mm_set1_epi32(INT32_MIN), g[ra].vi, _mm_set1_epi32(i10))
#define SPU_OP_CLGTHI SPU_CLGT(_mm_cmpgt_epi16, _mm_set1_epi16(-32768), g[ra].vi, _mm_set1_epi16(int16_t(i10)))
#define SPU_OP_CLGTBI SPU_CLGT(_mm_cmpgt_epi8, _mm_set1_epi8(-128), g[ra].vi, _mm_set1_epi8(s8(i10)))

// Halts (SPU threads treat a halt as a fatal emulation error).
#define SPU_OP_HEQ if (SPU_PREF(ra) == SPU_PREF(rb)) ::spu::ops::halt(t, "heq", pc, w)
#define SPU_OP_HGT if (s32(SPU_PREF(ra)) > s32(SPU_PREF(rb))) ::spu::ops::halt(t, "hgt", pc, w)
#define SPU_OP_HLGT if (SPU_PREF(ra) > SPU_PREF(rb)) ::spu::ops::halt(t, "hlgt", pc, w)
#define SPU_OP_HEQI if (s32(SPU_PREF(ra)) == i10) ::spu::ops::halt(t, "heqi", pc, w)
#define SPU_OP_HGTI if (s32(SPU_PREF(ra)) > i10) ::spu::ops::halt(t, "hgti", pc, w)
#define SPU_OP_HLGTI if (SPU_PREF(ra) > u32(i10)) ::spu::ops::halt(t, "hlgti", pc, w)

// Floating point (single precision runs under RZ + FTZ/DAZ set by the SPU host thread).
#define SPU_OP_FA g[rt].vf = _mm_add_ps(g[ra].vf, g[rb].vf)
#define SPU_OP_FS g[rt].vf = _mm_sub_ps(g[ra].vf, g[rb].vf)
#define SPU_OP_FM g[rt].vf = _mm_mul_ps(g[ra].vf, g[rb].vf)
#define SPU_OP_FREST g[rt].vf = _mm_div_ps(_mm_set1_ps(1.0f), g[ra].vf)
#define SPU_OP_FRSQEST \
  g[rt].vf = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), g[ra].vf)))
#define SPU_OP_FI g[rt] = g[rb]  // frest/frsqest already return the refined value
#define SPU_OP_FCEQ g[rt].vf = _mm_cmpeq_ps(g[ra].vf, g[rb].vf)
#define SPU_OP_FCGT g[rt].vf = _mm_cmpgt_ps(g[ra].vf, g[rb].vf)
#define SPU_OP_FCMEQ \
  g[rt].vf = _mm_cmpeq_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), g[ra].vf), _mm_andnot_ps(_mm_set1_ps(-0.0f), g[rb].vf))
#define SPU_OP_FCMGT \
  g[rt].vf = _mm_cmpgt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), g[ra].vf), _mm_andnot_ps(_mm_set1_ps(-0.0f), g[rb].vf))
#define SPU_OP_CFLTS                                                                                       \
  {                                                                                                        \
    double sc_ = std::ldexp(1.0, int(173 - ((w >> 14) & 0xff)));                                           \
    SPU_LOOP4S(([&] {                                                                                       \
      double x_ = double(g[ra].fx[k]) * sc_;                                                               \
      return std::isnan(x_) ? INT32_MIN : x_ >= 2147483647.0 ? INT32_MAX : x_ <= -2147483648.0 ? INT32_MIN : s32(x_); \
    }()))                                                                                                   \
  }
#define SPU_OP_CFLTU                                                                                       \
  {                                                                                                        \
    double sc_ = std::ldexp(1.0, int(173 - ((w >> 14) & 0xff)));                                           \
    SPU_LOOP4(([&] {                                                                                        \
      double x_ = double(g[ra].fx[k]) * sc_;                                                               \
      return std::isnan(x_) || x_ <= 0 ? 0u : x_ >= 4294967295.0 ? UINT32_MAX : u32(x_);                   \
    }()))                                                                                                   \
  }
#define SPU_OP_CSFLT                                                                                       \
  {                                                                                                        \
    float sc_ = std::ldexp(1.0f, int((w >> 14) & 0xff) - 155);                                             \
    ppu::v128 r_;                                                                                          \
    for (int k = 0; k < 4; ++k) r_.fx[k] = float(g[ra].s32x[k]) * sc_;                                      \
    g[rt] = r_;                                                                                            \
  }
#define SPU_OP_CUFLT                                                                                       \
  {                                                                                                        \
    float sc_ = std::ldexp(1.0f, int((w >> 14) & 0xff) - 155);                                             \
    ppu::v128 r_;                                                                                          \
    for (int k = 0; k < 4; ++k) r_.fx[k] = float(g[ra].u32x[k]) * sc_;                                      \
    g[rt] = r_;                                                                                            \
  }
#define SPU_PD(r) _mm_castsi128_pd(g[r].vi)
#define SPU_OP_DFA g[rt].vi = _mm_castpd_si128(_mm_add_pd(SPU_PD(ra), SPU_PD(rb)))
#define SPU_OP_DFS g[rt].vi = _mm_castpd_si128(_mm_sub_pd(SPU_PD(ra), SPU_PD(rb)))
#define SPU_OP_DFM g[rt].vi = _mm_castpd_si128(_mm_mul_pd(SPU_PD(ra), SPU_PD(rb)))
#define SPU_OP_DFMA g[rt].vi = _mm_castpd_si128(_mm_fmadd_pd(SPU_PD(ra), SPU_PD(rb), SPU_PD(rt)))
#define SPU_OP_DFMS g[rt].vi = _mm_castpd_si128(_mm_fmsub_pd(SPU_PD(ra), SPU_PD(rb), SPU_PD(rt)))
#define SPU_OP_DFNMS g[rt].vi = _mm_castpd_si128(_mm_fnmadd_pd(SPU_PD(ra), SPU_PD(rb), SPU_PD(rt)))
#define SPU_OP_DFNMA \
  g[rt].vi = _mm_castpd_si128(_mm_xor_pd(_mm_fmadd_pd(SPU_PD(ra), SPU_PD(rb), SPU_PD(rt)), _mm_set1_pd(-0.0)))
#define SPU_OP_FESD                                                                         \
  {                                                                                         \
    ppu::v128 r_;                                                                           \
    double d0_ = double(g[ra].fx[3]), d1_ = double(g[ra].fx[1]);                            \
    std::memcpy(&r_.u64x[1], &d0_, 8);                                                      \
    std::memcpy(&r_.u64x[0], &d1_, 8);                                                      \
    g[rt] = r_;                                                                             \
  }
#define SPU_OP_FRDS                                                                         \
  {                                                                                         \
    ppu::v128 r_{};                                                                         \
    double d0_, d1_;                                                                        \
    std::memcpy(&d0_, &g[ra].u64x[1], 8);                                                   \
    std::memcpy(&d1_, &g[ra].u64x[0], 8);                                                   \
    r_.fx[3] = float(d0_);                                                                  \
    r_.fx[1] = float(d1_);                                                                  \
    g[rt] = r_;                                                                             \
  }
#define SPU_OP_DFCEQ g[rt].vi = _mm_castpd_si128(_mm_cmpeq_pd(SPU_PD(ra), SPU_PD(rb)))
#define SPU_OP_DFCGT g[rt].vi = _mm_castpd_si128(_mm_cmpgt_pd(SPU_PD(ra), SPU_PD(rb)))
#define SPU_OP_DFCMEQ \
  g[rt].vi = _mm_castpd_si128(_mm_cmpeq_pd(_mm_andnot_pd(_mm_set1_pd(-0.0), SPU_PD(ra)), _mm_andnot_pd(_mm_set1_pd(-0.0), SPU_PD(rb))))
#define SPU_OP_DFCMGT \
  g[rt].vi = _mm_castpd_si128(_mm_cmpgt_pd(_mm_andnot_pd(_mm_set1_pd(-0.0), SPU_PD(ra)), _mm_andnot_pd(_mm_set1_pd(-0.0), SPU_PD(rb))))
#define SPU_OP_DFTSV                                                                        \
  {                                                                                         \
    u32 sel_ = u32(i7) & 0x7f;                                                              \
    ppu::v128 r_;                                                                           \
    for (int k = 0; k < 2; ++k) {                                                           \
      double d_;                                                                            \
      std::memcpy(&d_, &g[ra].u64x[k], 8);                                                  \
      int c_ = std::fpclassify(d_);                                                         \
      bool neg_ = std::signbit(d_), m_ = false;                                             \
      if ((sel_ & 0x40) && c_ == FP_NAN) m_ = true;                                         \
      if ((sel_ & 0x20) && c_ == FP_INFINITE && !neg_) m_ = true;                           \
      if ((sel_ & 0x10) && c_ == FP_INFINITE && neg_) m_ = true;                            \
      if ((sel_ & 0x08) && c_ == FP_ZERO && !neg_) m_ = true;                               \
      if ((sel_ & 0x04) && c_ == FP_ZERO && neg_) m_ = true;                                \
      if ((sel_ & 0x02) && c_ == FP_SUBNORMAL && !neg_) m_ = true;                          \
      if ((sel_ & 0x01) && c_ == FP_SUBNORMAL && neg_) m_ = true;                           \
      r_.u64x[k] = m_ ? ~0ull : 0;                                                          \
    }                                                                                       \
    g[rt] = r_;                                                                             \
  }
#define SPU_OP_FSCRRD g[rt] = ppu::v128{}
#define SPU_OP_FSCRWR
#define SPU_OP_MFSPR g[rt] = ppu::v128{}
#define SPU_OP_MTSPR

// X-macro list of every instruction implemented by an SPU_OP_<NAME> macro.
#define SPU_SIMPLE_OPS(X)                                                                                   \
  X(SELB) X(SHUFB) X(MPYA) X(FMA) X(FMS) X(FNMS) X(ILA) X(IL) X(ILH) X(ILHU) X(IOHL) X(FSMBI) X(HBRA) X(HBRR)   \
  X(HBR) X(LNOP) X(NOP) X(SYNC) X(DSYNC) X(LQD) X(LQX) X(LQA) X(LQR) X(STQD) X(STQX) X(STQA) X(STQR) X(CBD)     \
  X(CHD) X(CWD) X(CDD) X(CBX) X(CHX) X(CWX) X(CDX) X(A) X(AH) X(AI) X(AHI) X(SF) X(SFH) X(SFI) X(SFHI) X(CG)    \
  X(BG) X(ADDX) X(SFX) X(CGX) X(BGX) X(MPY) X(MPYU) X(MPYI) X(MPYUI) X(MPYH) X(MPYS) X(MPYHH) X(MPYHHU)         \
  X(MPYHHA) X(MPYHHAU) X(CLZ) X(CNTB) X(FSMB) X(FSMH) X(FSM) X(GBB) X(GBH) X(GB) X(AVGB) X(ABSDB) X(SUMB)       \
  X(XSBH) X(XSHW) X(XSWD) X(AND) X(ANDC) X(OR) X(ORC) X(XOR) X(NAND) X(NOR) X(EQV) X(ORX) X(ANDI) X(ANDHI)      \
  X(ANDBI) X(ORI) X(ORHI) X(ORBI) X(XORI) X(XORHI) X(XORBI) X(SHL) X(SHLI) X(SHLH) X(SHLHI) X(ROT) X(ROTI)       \
  X(ROTH) X(ROTHI) X(ROTM) X(ROTMI) X(ROTHM) X(ROTHMI) X(ROTMA) X(ROTMAI) X(ROTMAH) X(ROTMAHI) X(ROTQBY)         \
  X(ROTQBYI) X(ROTQBYBI) X(ROTQBI) X(ROTQBII) X(SHLQBY) X(SHLQBYI) X(SHLQBYBI) X(SHLQBI) X(SHLQBII) X(ROTQMBY)   \
  X(ROTQMBYI) X(ROTQMBYBI) X(ROTQMBI) X(ROTQMBII) X(CEQ) X(CEQH) X(CEQB) X(CEQI) X(CEQHI) X(CEQBI) X(CGT)        \
  X(CGTH) X(CGTB) X(CGTI) X(CGTHI) X(CGTBI) X(CLGT) X(CLGTH) X(CLGTB) X(CLGTI) X(CLGTHI) X(CLGTBI) X(HEQ) X(HGT) \
  X(HLGT) X(HEQI) X(HGTI) X(HLGTI) X(FA) X(FS) X(FM) X(FREST) X(FRSQEST) X(FI) X(FCEQ) X(FCGT) X(FCMEQ)         \
  X(FCMGT) X(CFLTS) X(CFLTU) X(CSFLT) X(CUFLT) X(DFA) X(DFS) X(DFM) X(DFMA) X(DFMS) X(DFNMS) X(DFNMA) X(FESD)    \
  X(FRDS) X(DFCEQ) X(DFCGT) X(DFCMEQ) X(DFCMGT) X(DFTSV) X(FSCRRD) X(FSCRWR) X(MFSPR) X(MTSPR)

// ---- support for recompiled code ----
// Defines the decoded fields of instruction word W at address PC as constants.
#define SPU_FIELDS(PC, W)                                                                                  \
  constexpr u32 pc = (PC), w = (W), rt = (W) & 0x7f, ra = ((W) >> 7) & 0x7f, rb = ((W) >> 14) & 0x7f,      \
                rrr_t = ((W) >> 21) & 0x7f, rrr_c = (W) & 0x7f, i16 = ((W) >> 7) & 0xffff,                 \
                i18 = ((W) >> 7) & 0x3ffff;                                                                \
  constexpr s32 i7 = s32(u32(W) << 11) >> 25, i10 = s32(u32(W) << 8) >> 22,                                \
                s16 = s32(int16_t(u16(((W) >> 7) & 0xffff)));
// Leaves the chunk to continue at `target` (dispatcher picks the next chunk).
#define SPU_EXIT(target) \
  {                      \
    t.pc = (target);     \
    return true;         \
  }
// Taken backward branch inside a chunk: give the dispatcher a chance to stop or deliver
// interrupts before looping.
#define SPU_BACKEDGE(target, label)                                                  \
  {                                                                                  \
    if (t.stop_request.load(std::memory_order_relaxed) || t.int_enabled) SPU_EXIT(target) \
    goto label;                                                                      \
  }
