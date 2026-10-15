// Shader warm-up from the shader manifest (ac4 --warm-shaders; see shader_warm.cpp).
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt::gpu {

struct WarmResult {
  u32 programs = 0;  // manifest entries
  u32 compiled = 0, cached = 0, skipped = 0, failed = 0;
  u32 threads = 0;
  double seconds = 0;
  std::string error;  // non-empty if the manifest could not be read
};

// Compiles the manifest's programs (read from the game folder `disc_root` and ac4-prep's output
// `prepared`) into the shader cache on all but one hardware thread. Needs no device.
WarmResult warm_shaders(const std::string& manifest, const std::string& disc_root, const std::string& prepared);

}  // namespace rt::gpu
