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

// Translates the program starting at `start`. The shader reads:
//   StructuredBuffer<float4> vtx : t0     (16 float4 attributes per vertex, SV_VertexID-indexed)
//   cbuffer VpConst : b0 { float4 vc[512]; }
//   cbuffer VpState : b1 { float4 vp_scale, vp_offset, rt_size; uint4 vp_flags; }  (rt_size.zw = 1/size)
// and writes SV_Position plus o1..o15 as TEXCOORD1..15.
std::string vp_to_hlsl(const u32* slots, u32 start, VpInfo* info);

}  // namespace rt::gpu
