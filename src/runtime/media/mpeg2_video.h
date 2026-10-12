// MPEG-2 video decoder (ISO/IEC 13818-2, Main Profile: 4:2:0, 8-bit; MPEG-1 streams too).
// Frame and field pictures, I/P/B, frame/field/16x8/dual-prime motion compensation. Pictures
// leave the decoder in display order with the standard reordering delay only: a B picture as
// soon as it is decoded, an I or P picture when the next I or P picture starts (or at drain).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::media {

struct Mpeg2Picture {
  u32 width = 0, height = 0;  // horizontal_size x vertical_size
  std::vector<u8> yuv;        // I420: Y (width*height), then U and V ((width+1)/2 x (height+1)/2)
  u64 tag = 0;                // tag of the access unit that carried the picture (its first field)
  int coding_type = 0;        // 1 I, 2 P, 3 B
};

class Mpeg2Decoder {
 public:
  Mpeg2Decoder();
  ~Mpeg2Decoder();

  // Decodes one access unit: any headers plus the picture (or field pair) it carries. Pictures
  // that become ready are appended to `out` in display order. With `skip_b`, B pictures are
  // parsed but not decoded (and not output). Returns false if the stream could not be decoded
  // (the reason is in error()); later access units may still decode.
  bool decode(const u8* data, size_t size, u64 tag, bool skip_b, std::vector<Mpeg2Picture>* out);
  // Outputs the reference picture still held for reordering (end of a sequence).
  void drain(std::vector<Mpeg2Picture>* out);
  // Forgets all pictures (a new sequence starts; the next access unit must not need references).
  void reset();

  bool have_sequence() const;
  const std::string& error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace rt::media
