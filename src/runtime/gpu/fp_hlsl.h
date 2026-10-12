// RSX fragment program (FP) microcode -> HLSL ps_5_0 (local\research\rsx_facts.md §2).
#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::gpu {

// State the translation depends on (part of the shader cache key).
struct FpEnv {
  u8 tex_dim[16] = {};      // 0 none, 1 1D, 2 2D, 3 3D, 4 cube (from the bound texture FORMAT)
  u8 mrt_count = 1;         // color outputs (1..4)
  bool fp32_exports = true; // SHADER_CONTROL bit 6: colors from R0/R2/R3/R4, else H0/H4/H6/H8
  bool depth_export = false;
  bool operator==(const FpEnv&) const = default;
};

struct FpInfo {
  u32 size = 0;          // ucode bytes (instructions + inline constants)
  u32 tex_mask = 0;      // texture units sampled
  bool uses_kil = false;
  std::vector<u32> const_offsets;  // byte offset of each inline constant slot -> fc[i]
  std::string error;
};

// Logical 32-bit word of FP ucode at `p` (stored as two big-endian halves, low half first).
inline u32 fp_word(const u8* p) { return (u32(p[0]) << 8 | p[1]) | (u32(p[2]) << 8 | p[3]) << 16; }

// Translates the program at `ucode` (at most `max_bytes` readable). The shader reads:
//   cbuffer FpState : b2 { float4 fp_wpos; uint4 fp_flags; float4 fp_alpha; float4 fp_fog; float4 tex_scale[16]; }
//     fp_wpos = (window height, origin bottom (0/1), pixel center integer (0/1), two-sided (0/1))
//     fp_flags.x = TEX_COORD_CONTROL 2D mask, .y = alpha test func (0 = off), fp_alpha.x = alpha ref
//     fp_fog = (mode, p0, p1, 0); tex_scale[i] = (1/w, 1/h, 1/d, unnormalized flag)
//   cbuffer FpConst : b3 { float4 fc[256]; }  inline constants, in order (info->const_offsets)
//   textures t0..t15 / samplers s0..s15, and the VS outputs TEXCOORD0..15 (TEXCOORD0 = clip pos).
std::string fp_to_hlsl(const u8* ucode, u32 max_bytes, const FpEnv& env, FpInfo* info);

// Scans the program: total size in bytes (0 if no END within max_bytes), a hash of the
// instruction words (inline constants excluded) and the constant slot offsets.
u32 fp_scan(const u8* ucode, u32 max_bytes, u64* code_hash, std::vector<u32>* const_offsets);

}  // namespace rt::gpu
