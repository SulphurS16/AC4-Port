#include "runtime/gpu/png.h"

#include <cstdio>

#include "common/file.h"
#include "zlib.h"

namespace rt::gpu {

bool write_png(const std::string& path, u32 w, u32 h, const std::vector<u8>& rgba) {
  std::vector<u8> raw;
  raw.reserve(size_t(h) * (w * 4 + 1));
  for (u32 y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
  }
  uLongf zlen = compressBound(uLong(raw.size()));
  std::vector<u8> z(zlen);
  if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
  z.resize(zlen);
  std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  auto chunk = [&](const char* type, const std::vector<u8>& data) {
    u8 len[4];
    ac4::wr_be32(len, u32(data.size()));
    out.insert(out.end(), len, len + 4);
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    u32 crc = u32(crc32(0, out.data() + start, uInt(out.size() - start)));
    u8 c[4];
    ac4::wr_be32(c, crc);
    out.insert(out.end(), c, c + 4);
  };
  std::vector<u8> ihdr(13);
  ac4::wr_be32(&ihdr[0], w);
  ac4::wr_be32(&ihdr[4], h);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 6;   // RGBA
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  try {
    ac4::write_file(path, out);
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace rt::gpu
