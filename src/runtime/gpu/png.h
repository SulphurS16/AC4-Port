#pragma once
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::gpu {

// Writes 8-bit RGBA pixels (row-major, tightly packed) as a PNG file. Returns false on failure.
bool write_png(const std::string& path, u32 w, u32 h, const std::vector<u8>& rgba);

}  // namespace rt::gpu
