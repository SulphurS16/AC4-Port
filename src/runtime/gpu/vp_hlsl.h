// RSX vertex program (VP) microcode -> HLSL vs_5_0 (local\research\rsx_facts.md §1).
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt::gpu {

constexpr u32 kVpSlots = 544;  // transform program memory, 4 words per instruction

struct VpInfo {
  u32 input_mask = 0;    // vertex attributes read
  u32 output_mask = 0;   // o[] registers written
  u32 first = 0, last = 0;
  bool branches = false;
  std::string error;     // non-empty if the program could not be translated
};

// Vertex attribute descriptors (cbuffer VtxFmt : b2), one per input register, filled by the
// renderer per draw. Arrays are read from raw guest bytes (big-endian) in ByteAddressBuffers
// t0..t3 and decoded in the shader: element k' = (stride ? (freq > 1 ? (modulo ? k % freq :
// k / freq) : k) : 0) for the draw's element index k (SV_VertexID), address base + k' * stride.
struct VtxAttr {
  u32 base;    // byte address of element 0 in the attribute's buffer (wraps: may lie before the data)
  u32 stride;  // bytes between elements
  u32 freq;    // frequency divider (0/1: none)
  u32 fmt;     // type | size << 4 | buffer << 8 | modulo << 12 | array << 16 (array 0: the constant)
};
struct VtxFmt {
  VtxAttr attr[16];
  float konst[16][4];  // inputs without an array: the register values
};
constexpr u32 kVtxBuffers = 4;

// Translates the program starting at `start`. The shader reads:
//   ByteAddressBuffer vb0..vb3 : t0..t3   (raw vertex streams), cbuffer VtxFmt : b2
//   cbuffer VpConst : b0 { float4 vc[512]; }
//   cbuffer VpState : b1 { float4 vp_scale, vp_offset, rt_size; uint4 vp_flags; }  (rt_size.zw = 1/size)
// and writes SV_Position plus o1..o15 as TEXCOORD1..15.
std::string vp_to_hlsl(const u32* slots, u32 start, VpInfo* info);

}  // namespace rt::gpu
