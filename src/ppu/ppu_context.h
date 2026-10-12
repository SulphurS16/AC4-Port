// Guest PPU register context shared by the runtime, generated code and the interpreter.
#pragma once
#include <immintrin.h>

#include <cstdint>
#include <cstring>

#include "common/bytes.h"

namespace ppu {

// 128-bit VMX register. Stored byte-reversed relative to the guest: host byte j holds guest
// (big-endian) byte 15-j, so the host little-endian 128-bit integer equals the guest's
// big-endian 128-bit integer, and guest element i of an N-element vector is host element N-1-i.
union alignas(16) v128 {
  u8 u8x[16];
  s8 s8x[16];
  u16 u16x[8];
  s16 s16x[8];
  u32 u32x[4];
  s32 s32x[4];
  u64 u64x[2];
  float fx[4];
  __m128i vi;
  __m128 vf;
};
static_assert(sizeof(v128) == 16);

// Guest element accessors (big-endian element numbering).
inline u8& be8(v128& v, int i) { return v.u8x[15 - i]; }
inline u16& be16(v128& v, int i) { return v.u16x[7 - i]; }
inline u32& be32(v128& v, int i) { return v.u32x[3 - i]; }
inline float& bef(v128& v, int i) { return v.fx[3 - i]; }

struct alignas(64) Context {
  u64 r[32];
  double f[32];
  v128 v[32];
  u64 lr, ctr;
  u8 cr[32];  // one byte (0/1) per CR bit; field n bits are cr[4n+0..3] = LT, GT, EQ, SO
  u8 so, ov, ca, xer_cnt;
  u32 fpscr;
  u32 vscr;  // bit 16 = NJ, bit 0 = SAT
  u32 vrsave;
  u32 res_addr;  // lwarx/ldarx reservation address (0 = none)
  u64 res_val;   // reserved value (as read, guest byte order normalized)
  u32 res_size;
  u32 cia;      // last recorded guest address (diagnostics)
  void* thread; // runtime thread object
  u64 pad_[4];
  // The reservation granule (the 128-byte line holding res_addr) as lwarx/ldarx read it. As on
  // the Cell PPU, a change anywhere in the granule since then makes stwcx/stdcx fail.
  alignas(64) u8 res_line[128];
};

}  // namespace ppu
