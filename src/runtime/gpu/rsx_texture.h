// RSX texture decoding (rsx_facts.md §5): register decode and conversion of guest texture memory
// into Direct3D 11 upload data.
#pragma once
#include <dxgiformat.h>

#include <vector>

#include "common/bytes.h"

namespace rt::gpu {

struct TexDesc {
  u32 unit = 0;
  u32 offset = 0, location = 0;  // location 0 local, 1 main
  u32 format = 0;                // base format (LN/UN cleared)
  bool linear = false, unnormalized = false, cubemap = false, border_color = false;
  u32 dim = 2;                   // 1, 2, 3
  u32 width = 0, height = 0, depth = 1, mips = 1, pitch = 0;
  u32 remap = 0xAAE4;            // CONTROL1
  u32 address = 0, filter = 0, control0 = 0, border = 0;
  bool enabled = false;
};

// Decodes texture unit `unit` (0..15) from the register file.
TexDesc decode_texture(const u32* regs, u32 unit);

// Bytes needed by the texture in guest memory (all faces and levels), 0 if unsupported.
u32 texture_size(const TexDesc& t);

struct TexData {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  u32 width = 0, height = 0, depth = 1, mips = 1, faces = 1;
  struct Level {
    std::vector<u8> data;
    u32 row_pitch = 0, slice_pitch = 0;
  };
  std::vector<Level> levels;  // face-major: levels[face * mips + level]
  std::string error;
};

// Converts the texture at `src` (texture_size(t) bytes) for upload. Channel remap (CONTROL1) is
// applied for 8-bit formats.
bool convert_texture(const TexDesc& t, const u8* src, TexData* out);

}  // namespace rt::gpu
