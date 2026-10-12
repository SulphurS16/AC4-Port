// MPEG-2 video decoder (ISO/IEC 13818-2 / ITU-T H.262), Main Profile. Section numbers refer to
// H.262. MPEG-1 (ISO/IEC 11172-2) streams are decoded with their differences (6.3 / 7.4).
#include "runtime/media/mpeg2_video.h"

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace rt::media {
namespace {

// ---- bit reader (6.1.1: most significant bit first) ----
// `cache_` holds the next bits left-aligned; at least 32 valid bits after every refill.
class Bits {
 public:
  Bits(const u8* p, size_t n) : p_(p), n_(n) { refill(); }
  u32 peek(int k) const { return u32(cache_ >> (64 - k)); }
  void skip(int k) {
    cache_ <<= k;
    avail_ -= k;
    consumed_ += size_t(k);
    if (avail_ < 32) refill();
  }
  u32 get(int k) {
    u32 v = peek(k);
    skip(k);
    return v;
  }
  bool past_end() const { return consumed_ > n_ * 8; }
  bool next_is_start_code() const { return peek(23) == 0; }  // zero bits up to the next 0x000001

 private:
  void refill() {
    // Append whole bytes below the valid bits (bytes past the end read as zero).
    while (avail_ <= 56) {
      u64 byte = next_ < n_ ? p_[next_] : 0;
      ++next_;
      cache_ |= byte << (56 - avail_);
      avail_ += 8;
    }
  }
  const u8* p_;
  size_t n_, next_ = 0, consumed_ = 0;
  u64 cache_ = 0;
  int avail_ = 0;
};

// ---- variable length codes (Annex B) ----
struct VlcEntry {
  s16 value;  // decoded value; kInvalid if no code matches
  u8 len;
};
constexpr s16 kInvalid = -32768;

// Builds a lookup table indexed by the next `bits` bits from (code-string, value) pairs and
// verifies that the code is prefix-free.
template <size_t N>
std::vector<VlcEntry> build_vlc(int bits, const std::array<std::pair<const char*, int>, N>& codes) {
  std::vector<VlcEntry> t(size_t(1) << bits, VlcEntry{kInvalid, 0});
  for (const auto& [s, v] : codes) {
    int len = int(std::strlen(s));
    u32 code = 0;
    for (int i = 0; i < len; ++i) code = code << 1 | u32(s[i] == '1');
    u32 first = code << (bits - len), count = 1u << (bits - len);
    for (u32 k = 0; k < count; ++k) {
      if (t[first + k].len) throw std::logic_error(std::string("mpeg2: VLC table not prefix-free at ") + s);
      t[first + k] = VlcEntry{s16(v), u8(len)};
    }
  }
  return t;
}

int read_vlc(Bits& b, const std::vector<VlcEntry>& t, int bits) {
  const VlcEntry& e = t[b.peek(bits)];
  if (e.value == kInvalid) return kInvalid;
  b.skip(e.len);
  return e.value;
}

// B.1 macroblock_address_increment (34 = escape, 35 = stuffing).
const std::vector<VlcEntry>& mba_table() {
  static const auto t = build_vlc<35>(11, {{
      {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"00011", 6}, {"00010", 7}, {"0000111", 8},
      {"0000110", 9}, {"00001011", 10}, {"00001010", 11}, {"00001001", 12}, {"00001000", 13}, {"00000111", 14},
      {"00000110", 15}, {"0000010111", 16}, {"0000010110", 17}, {"0000010101", 18}, {"0000010100", 19},
      {"0000010011", 20}, {"0000010010", 21}, {"00000100011", 22}, {"00000100010", 23}, {"00000100001", 24},
      {"00000100000", 25}, {"00000011111", 26}, {"00000011110", 27}, {"00000011101", 28}, {"00000011100", 29},
      {"00000011011", 30}, {"00000011010", 31}, {"00000011001", 32}, {"00000011000", 33}, {"00000001000", 34},
      {"00000001111", 35},
  }});
  return t;
}

// macroblock_type flags (6.3.17.1).
enum : int { MB_QUANT = 1, MB_FWD = 2, MB_BWD = 4, MB_PATTERN = 8, MB_INTRA = 16 };

// B.2 / B.3 / B.4 macroblock_type for I, P and B pictures.
const std::vector<VlcEntry>& mbtype_table(int coding_type) {
  static const auto ti = build_vlc<2>(6, {{{"1", MB_INTRA}, {"01", MB_INTRA | MB_QUANT}}});
  static const auto tp = build_vlc<7>(6, {{
      {"1", MB_FWD | MB_PATTERN}, {"01", MB_PATTERN}, {"001", MB_FWD}, {"00011", MB_INTRA},
      {"00010", MB_FWD | MB_PATTERN | MB_QUANT}, {"00001", MB_PATTERN | MB_QUANT}, {"000001", MB_INTRA | MB_QUANT},
  }});
  static const auto tb = build_vlc<11>(6, {{
      {"10", MB_FWD | MB_BWD}, {"11", MB_FWD | MB_BWD | MB_PATTERN}, {"010", MB_BWD}, {"011", MB_BWD | MB_PATTERN},
      {"0010", MB_FWD}, {"0011", MB_FWD | MB_PATTERN}, {"00011", MB_INTRA}, {"00010", MB_FWD | MB_BWD | MB_PATTERN | MB_QUANT},
      {"000011", MB_FWD | MB_PATTERN | MB_QUANT}, {"000010", MB_BWD | MB_PATTERN | MB_QUANT}, {"000001", MB_INTRA | MB_QUANT},
  }});
  static const auto td = build_vlc<1>(6, {{{"1", MB_INTRA}}});  // MPEG-1 D pictures
  return coding_type == 1 ? ti : coding_type == 2 ? tp : coding_type == 3 ? tb : td;
}

// B.9 coded_block_pattern (4:2:0).
const std::vector<VlcEntry>& cbp_table() {
  static const auto t = build_vlc<64>(9, {{
      {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"10011", 12}, {"10010", 48}, {"10001", 20},
      {"10000", 40}, {"01111", 28}, {"01110", 44}, {"01101", 52}, {"01100", 56}, {"01011", 1}, {"01010", 61}, {"01001", 2},
      {"01000", 62}, {"001111", 24}, {"001110", 36}, {"001101", 3}, {"001100", 63}, {"0010111", 5}, {"0010110", 9},
      {"0010101", 17}, {"0010100", 33}, {"0010011", 6}, {"0010010", 10}, {"0010001", 18}, {"0010000", 34},
      {"00011111", 7}, {"00011110", 11}, {"00011101", 19}, {"00011100", 35}, {"00011011", 13}, {"00011010", 49},
      {"00011001", 21}, {"00011000", 41}, {"00010111", 14}, {"00010110", 50}, {"00010101", 22}, {"00010100", 42},
      {"00010011", 15}, {"00010010", 51}, {"00010001", 23}, {"00010000", 43}, {"00001111", 25}, {"00001110", 37},
      {"00001101", 26}, {"00001100", 38}, {"00001011", 29}, {"00001010", 45}, {"00001001", 53}, {"00001000", 57},
      {"00000111", 30}, {"00000110", 46}, {"00000101", 54}, {"00000100", 58}, {"000000111", 31}, {"000000110", 47},
      {"000000101", 55}, {"000000100", 59}, {"000000011", 27}, {"000000010", 39}, {"000000001", 0},
  }});
  return t;
}

// B.10 motion_code magnitude (the sign bit follows the code for non-zero values).
const std::vector<VlcEntry>& motion_table() {
  static const auto t = build_vlc<17>(10, {{
      {"1", 0}, {"01", 1}, {"001", 2}, {"0001", 3}, {"000011", 4}, {"0000101", 5}, {"0000100", 6}, {"0000011", 7},
      {"000001011", 8}, {"000001010", 9}, {"000001001", 10}, {"0000010001", 11}, {"0000010000", 12},
      {"0000001111", 13}, {"0000001110", 14}, {"0000001101", 15}, {"0000001100", 16},
  }});
  return t;
}

// B.11 dmvector.
const std::vector<VlcEntry>& dmv_table() {
  static const auto t = build_vlc<3>(2, {{{"0", 0}, {"10", 1}, {"11", -1}}});
  return t;
}

// B.12 / B.13 dct_dc_size for luminance and chrominance.
const std::vector<VlcEntry>& dc_size_table(bool chroma) {
  static const auto tl = build_vlc<12>(9, {{
      {"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5}, {"11110", 6}, {"111110", 7},
      {"1111110", 8}, {"11111110", 9}, {"111111110", 10}, {"111111111", 11},
  }});
  static const auto tc = build_vlc<12>(10, {{
      {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"11110", 5}, {"111110", 6}, {"1111110", 7},
      {"11111110", 8}, {"111111110", 9}, {"1111111110", 10}, {"1111111111", 11},
  }});
  return chroma ? tc : tl;
}

// B.14 / B.15 DCT coefficients: value = run << 8 | level; the sign bit follows the code.
constexpr int kEob = 0x7ffe, kEscape = 0x7fff;
constexpr int RL(int run, int level) { return run << 8 | level; }

// Codes of 14 to 16 bits are shared by both tables.
#define DCT_COMMON_LONG_CODES                                                                                          \
  {"00000000011111", RL(0, 16)}, {"00000000011110", RL(0, 17)}, {"00000000011101", RL(0, 18)},                         \
      {"00000000011100", RL(0, 19)}, {"00000000011011", RL(0, 20)}, {"00000000011010", RL(0, 21)},                     \
      {"00000000011001", RL(0, 22)}, {"00000000011000", RL(0, 23)}, {"00000000010111", RL(0, 24)},                     \
      {"00000000010110", RL(0, 25)}, {"00000000010101", RL(0, 26)}, {"00000000010100", RL(0, 27)},                     \
      {"00000000010011", RL(0, 28)}, {"00000000010010", RL(0, 29)}, {"00000000010001", RL(0, 30)},                     \
      {"00000000010000", RL(0, 31)}, {"000000000011000", RL(0, 32)}, {"000000000010111", RL(0, 33)},                   \
      {"000000000010110", RL(0, 34)}, {"000000000010101", RL(0, 35)}, {"000000000010100", RL(0, 36)},                  \
      {"000000000010011", RL(0, 37)}, {"000000000010010", RL(0, 38)}, {"000000000010001", RL(0, 39)},                  \
      {"000000000010000", RL(0, 40)}, {"000000000011111", RL(1, 8)}, {"000000000011110", RL(1, 9)},                    \
      {"000000000011101", RL(1, 10)}, {"000000000011100", RL(1, 11)}, {"000000000011011", RL(1, 12)},                  \
      {"000000000011010", RL(1, 13)}, {"000000000011001", RL(1, 14)}, {"0000000000010011", RL(1, 15)},                 \
      {"0000000000010010", RL(1, 16)}, {"0000000000010001", RL(1, 17)}, {"0000000000010000", RL(1, 18)},               \
      {"0000000000010100", RL(6, 3)}, {"0000000000011010", RL(11, 2)}, {"0000000000011001", RL(12, 2)},                \
      {"0000000000011000", RL(13, 2)}, {"0000000000010111", RL(14, 2)}, {"0000000000010110", RL(15, 2)},               \
      {"0000000000010101", RL(16, 2)}, {"0000000000011111", RL(27, 1)}, {"0000000000011110", RL(28, 1)},               \
      {"0000000000011101", RL(29, 1)}, {"0000000000011100", RL(30, 1)}, {"0000000000011011", RL(31, 1)}

const std::vector<VlcEntry>& dct_table_full(bool one) {
  static const auto t0 = build_vlc<113>(16, {{
      {"10", kEob}, {"11", RL(0, 1)}, {"011", RL(1, 1)}, {"0100", RL(0, 2)}, {"0101", RL(2, 1)}, {"00101", RL(0, 3)},
      {"00111", RL(3, 1)}, {"00110", RL(4, 1)}, {"000110", RL(1, 2)}, {"000111", RL(5, 1)}, {"000101", RL(6, 1)},
      {"000100", RL(7, 1)}, {"0000110", RL(0, 4)}, {"0000100", RL(2, 2)}, {"0000111", RL(8, 1)}, {"0000101", RL(9, 1)},
      {"000001", kEscape}, {"00100110", RL(0, 5)}, {"00100001", RL(0, 6)}, {"00100101", RL(1, 3)},
      {"00100100", RL(3, 2)}, {"00100111", RL(10, 1)}, {"00100011", RL(11, 1)}, {"00100010", RL(12, 1)},
      {"00100000", RL(13, 1)}, {"0000001010", RL(0, 7)}, {"0000001100", RL(1, 4)}, {"0000001011", RL(2, 3)},
      {"0000001111", RL(4, 2)}, {"0000001001", RL(5, 2)}, {"0000001110", RL(14, 1)}, {"0000001101", RL(15, 1)},
      {"0000001000", RL(16, 1)}, {"000000011101", RL(0, 8)}, {"000000011000", RL(0, 9)}, {"000000010011", RL(0, 10)},
      {"000000010000", RL(0, 11)}, {"000000011011", RL(1, 5)}, {"000000010100", RL(2, 4)}, {"000000011100", RL(3, 3)},
      {"000000010010", RL(4, 3)}, {"000000011110", RL(6, 2)}, {"000000010101", RL(7, 2)}, {"000000010001", RL(8, 2)},
      {"000000011111", RL(17, 1)}, {"000000011010", RL(18, 1)}, {"000000011001", RL(19, 1)},
      {"000000010111", RL(20, 1)}, {"000000010110", RL(21, 1)}, {"0000000011010", RL(0, 12)},
      {"0000000011001", RL(0, 13)}, {"0000000011000", RL(0, 14)}, {"0000000010111", RL(0, 15)},
      {"0000000010110", RL(1, 6)}, {"0000000010101", RL(1, 7)}, {"0000000010100", RL(2, 5)}, {"0000000010011", RL(3, 4)},
      {"0000000010010", RL(5, 3)}, {"0000000010001", RL(9, 2)}, {"0000000010000", RL(10, 2)},
      {"0000000011111", RL(22, 1)}, {"0000000011110", RL(23, 1)}, {"0000000011101", RL(24, 1)},
      {"0000000011100", RL(25, 1)}, {"0000000011011", RL(26, 1)}, DCT_COMMON_LONG_CODES,
  }});
  static const auto t1 = build_vlc<113>(16, {{
      {"0110", kEob}, {"10", RL(0, 1)}, {"010", RL(1, 1)}, {"110", RL(0, 2)}, {"00101", RL(2, 1)}, {"0111", RL(0, 3)},
      {"00111", RL(3, 1)}, {"000110", RL(4, 1)}, {"00110", RL(1, 2)}, {"000111", RL(5, 1)}, {"0000110", RL(6, 1)},
      {"0000100", RL(7, 1)}, {"11100", RL(0, 4)}, {"0000111", RL(2, 2)}, {"0000101", RL(8, 1)}, {"1111000", RL(9, 1)},
      {"000001", kEscape}, {"11101", RL(0, 5)}, {"000101", RL(0, 6)}, {"1111001", RL(1, 3)}, {"00100110", RL(3, 2)},
      {"1111010", RL(10, 1)}, {"00100001", RL(11, 1)}, {"00100101", RL(12, 1)}, {"00100100", RL(13, 1)},
      {"000100", RL(0, 7)}, {"00100111", RL(1, 4)}, {"11111100", RL(2, 3)}, {"11111101", RL(4, 2)},
      {"000000100", RL(5, 2)}, {"000000101", RL(14, 1)}, {"000000111", RL(15, 1)}, {"0000001101", RL(16, 1)},
      {"1111011", RL(0, 8)}, {"1111100", RL(0, 9)}, {"00100011", RL(0, 10)}, {"00100010", RL(0, 11)},
      {"00100000", RL(1, 5)}, {"0000001100", RL(2, 4)}, {"000000011100", RL(3, 3)}, {"000000010010", RL(4, 3)},
      {"000000011110", RL(6, 2)}, {"000000010101", RL(7, 2)}, {"000000010001", RL(8, 2)},
      {"000000011111", RL(17, 1)}, {"000000011010", RL(18, 1)}, {"000000011001", RL(19, 1)},
      {"000000010111", RL(20, 1)}, {"000000010110", RL(21, 1)}, {"11111010", RL(0, 12)}, {"11111011", RL(0, 13)},
      {"11111110", RL(0, 14)}, {"11111111", RL(0, 15)}, {"0000000010110", RL(1, 6)}, {"0000000010101", RL(1, 7)},
      {"0000000010100", RL(2, 5)}, {"0000000010011", RL(3, 4)}, {"0000000010010", RL(5, 3)},
      {"0000000010001", RL(9, 2)}, {"0000000010000", RL(10, 2)}, {"0000000011111", RL(22, 1)},
      {"0000000011110", RL(23, 1)}, {"0000000011101", RL(24, 1)}, {"0000000011100", RL(25, 1)},
      {"0000000011011", RL(26, 1)}, DCT_COMMON_LONG_CODES,
  }});
  return one ? t1 : t0;
}

// Two-level form of the coefficient tables: every code with at most six leading zeros is at most
// 10 bits long, so peek(16) >= 512 resolves through a 1024-entry table (index peek(16) >> 6) and
// the rest through a 512-entry table (index peek(16)). Both stay in the L1 cache.
struct DctTables {
  VlcEntry l1[1024], l2[512];
};
const DctTables& dct_table(bool one) {
  static const auto make = [](bool o) {
    DctTables t;
    const auto& full = dct_table_full(o);
    for (u32 i = 0; i < 1024; ++i) t.l1[i] = full[i << 6];
    for (u32 i = 0; i < 512; ++i) t.l2[i] = full[i];
    for (u32 i = 512; i < 65536; ++i)  // the split assumption: long codes all have 7+ leading zeros
      if (full[i].len > 10) throw std::logic_error("mpeg2: coefficient code longer than 10 bits above the split");
    return t;
  };
  static const DctTables t0 = make(false), t1 = make(true);
  return one ? t1 : t0;
}

// ---- scans and matrices (7.3, 6.3.11) ----
// Raster layouts of the two scans: element = scan index of that position (Figure 7-2, 7-3).
constexpr u8 kZigzagRaster[64] = {0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30,
                                  41, 43, 9,  11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38,
                                  46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
constexpr u8 kAlternateRaster[64] = {0,  4,  6,  20, 22, 36, 38, 52, 1,  5,  7,  21, 23, 37, 39, 53, 2,  8,  19, 24, 34, 40,
                                     50, 54, 3,  9,  18, 25, 35, 41, 51, 55, 10, 17, 26, 30, 42, 46, 56, 60, 11, 16, 27, 31,
                                     43, 47, 57, 61, 12, 15, 28, 32, 44, 48, 58, 62, 13, 14, 29, 33, 45, 49, 59, 63};
// Default intra matrix, raster order (6.3.11).
constexpr u8 kDefaultIntra[64] = {8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
                                  34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
                                  35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
// Non-linear quantiser_scale (Table 7-6).
constexpr u8 kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

struct Scans {
  u8 order[2][64];  // scan index -> raster position
  Scans() {
    for (int i = 0; i < 64; ++i) {
      order[0][kZigzagRaster[i]] = u8(i);
      order[1][kAlternateRaster[i]] = u8(i);
    }
  }
};
const Scans& scans() {
  static const Scans s;
  return s;
}

// ---- inverse DCT (Annex A) ----
// Separable IDCT on the Loeffler-Ligtenberg-Moschytz factorisation (12 multiplies per 1-D
// transform) in single precision, eight 1-D transforms at once in AVX registers. The 2-D
// normalisation (1/4 C(u) C(v)) is the same as the JPEG IDCT's; results are rounded and saturated
// to [-256, 255] (7.5).
inline void transpose8(__m256 r[8]) {
  __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]), t1 = _mm256_unpackhi_ps(r[0], r[1]);
  __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]), t3 = _mm256_unpackhi_ps(r[2], r[3]);
  __m256 t4 = _mm256_unpacklo_ps(r[4], r[5]), t5 = _mm256_unpackhi_ps(r[4], r[5]);
  __m256 t6 = _mm256_unpacklo_ps(r[6], r[7]), t7 = _mm256_unpackhi_ps(r[6], r[7]);
  __m256 s0 = _mm256_shuffle_ps(t0, t2, 0x44), s1 = _mm256_shuffle_ps(t0, t2, 0xEE);
  __m256 s2 = _mm256_shuffle_ps(t1, t3, 0x44), s3 = _mm256_shuffle_ps(t1, t3, 0xEE);
  __m256 s4 = _mm256_shuffle_ps(t4, t6, 0x44), s5 = _mm256_shuffle_ps(t4, t6, 0xEE);
  __m256 s6 = _mm256_shuffle_ps(t5, t7, 0x44), s7 = _mm256_shuffle_ps(t5, t7, 0xEE);
  r[0] = _mm256_permute2f128_ps(s0, s4, 0x20);
  r[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
  r[2] = _mm256_permute2f128_ps(s2, s6, 0x20);
  r[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
  r[4] = _mm256_permute2f128_ps(s0, s4, 0x31);
  r[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
  r[6] = _mm256_permute2f128_ps(s2, s6, 0x31);
  r[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
}

// in[k] holds coefficient k of eight vectors; out[x] receives sample x of each.
inline void idct_1d(const __m256 in[8], __m256 out[8]) {
  const __m256 k0298 = _mm256_set1_ps(0.298631336f), k0390 = _mm256_set1_ps(-0.390180644f),
               k0541 = _mm256_set1_ps(0.541196100f), k0765 = _mm256_set1_ps(0.765366865f),
               k0899 = _mm256_set1_ps(-0.899976223f), k1175 = _mm256_set1_ps(1.175875602f),
               k1501 = _mm256_set1_ps(1.501321110f), k1847 = _mm256_set1_ps(-1.847759065f),
               k1961 = _mm256_set1_ps(-1.961570560f), k2053 = _mm256_set1_ps(2.053119869f),
               k2562 = _mm256_set1_ps(-2.562915447f), k3072 = _mm256_set1_ps(3.072711026f);
  __m256 z1 = _mm256_mul_ps(_mm256_add_ps(in[2], in[6]), k0541);
  __m256 t2 = _mm256_fmadd_ps(in[6], k1847, z1), t3 = _mm256_fmadd_ps(in[2], k0765, z1);
  __m256 t0 = _mm256_add_ps(in[0], in[4]), t1 = _mm256_sub_ps(in[0], in[4]);
  __m256 e10 = _mm256_add_ps(t0, t3), e13 = _mm256_sub_ps(t0, t3), e11 = _mm256_add_ps(t1, t2), e12 = _mm256_sub_ps(t1, t2);
  __m256 o0 = in[7], o1 = in[5], o2 = in[3], o3 = in[1];
  __m256 a1 = _mm256_add_ps(o0, o3), a2 = _mm256_add_ps(o1, o2), a3 = _mm256_add_ps(o0, o2), a4 = _mm256_add_ps(o1, o3);
  __m256 a5 = _mm256_mul_ps(_mm256_add_ps(a3, a4), k1175);
  a3 = _mm256_fmadd_ps(a3, k1961, a5);
  a4 = _mm256_fmadd_ps(a4, k0390, a5);
  a1 = _mm256_mul_ps(a1, k0899);
  a2 = _mm256_mul_ps(a2, k2562);
  o0 = _mm256_fmadd_ps(o0, k0298, _mm256_add_ps(a1, a3));
  o1 = _mm256_fmadd_ps(o1, k2053, _mm256_add_ps(a2, a4));
  o2 = _mm256_fmadd_ps(o2, k3072, _mm256_add_ps(a2, a3));
  o3 = _mm256_fmadd_ps(o3, k1501, _mm256_add_ps(a1, a4));
  out[0] = _mm256_add_ps(e10, o3);
  out[7] = _mm256_sub_ps(e10, o3);
  out[1] = _mm256_add_ps(e11, o2);
  out[6] = _mm256_sub_ps(e11, o2);
  out[2] = _mm256_add_ps(e12, o1);
  out[5] = _mm256_sub_ps(e12, o1);
  out[3] = _mm256_add_ps(e13, o0);
  out[4] = _mm256_sub_ps(e13, o0);
}

// F (raster order) -> f. `nonzero_rows` has bit r set when row r holds any coefficient.
void idct(const int* F, int* f, u32 nonzero_rows) {
  if (nonzero_rows <= 1) {
    bool dc_only = true;
    for (int u = 1; u < 8; ++u) dc_only &= F[u] == 0;
    if (dc_only) {  // constant block: F[0] / 8, rounded
      int v = std::clamp(int(std::floor(float(F[0]) * 0.125f + 0.5f)), -256, 255);
      for (int k = 0; k < 64; ++k) f[k] = v;
      return;
    }
  }
  __m256 r[8], o[8];
  for (int y = 0; y < 8; ++y) r[y] = _mm256_cvtepi32_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(F + 8 * y)));
  transpose8(r);  // r[u]: coefficient u of every row
  idct_1d(r, o);  // o[x]: sample x of every row
  transpose8(o);  // o[v]: row v, as coefficients of every column
  idct_1d(o, r);  // r[y]: output row y (scaled by 8)
  const __m256 eighth = _mm256_set1_ps(0.125f), half = _mm256_set1_ps(0.5f);
  const __m256i lo = _mm256_set1_epi32(-256), hi = _mm256_set1_epi32(255);
  for (int y = 0; y < 8; ++y) {
    __m256i v = _mm256_cvttps_epi32(_mm256_floor_ps(_mm256_fmadd_ps(r[y], eighth, half)));
    v = _mm256_min_epi32(_mm256_max_epi32(v, lo), hi);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(f + 8 * y), v);
  }
}

inline u8 clamp_pixel(int v) { return u8(v < 0 ? 0 : v > 255 ? 255 : v); }

// ---- pictures ----
struct Frame {
  std::vector<u8> plane[3];
  int stride[3] = {}, w[3] = {}, h[3] = {};
  u64 tag = 0;
  int type = 0;
  bool output = false;
  void alloc(int luma_w, int luma_h) {
    for (int c = 0; c < 3; ++c) {
      w[c] = c ? luma_w / 2 : luma_w;
      h[c] = c ? luma_h / 2 : luma_h;
      stride[c] = w[c];
      plane[c].assign(size_t(w[c]) * h[c], 128);
    }
  }
};

// A view of a frame or one of its fields.
struct Plane {
  const u8* p = nullptr;
  int stride = 0, w = 0, h = 0;
};
Plane view(const Frame& f, int c, int parity /*-1 frame, 0 top, 1 bottom*/) {
  Plane v;
  if (parity < 0) {
    v.p = f.plane[c].data();
    v.stride = f.stride[c];
    v.h = f.h[c];
  } else {
    v.p = f.plane[c].data() + parity * f.stride[c];
    v.stride = 2 * f.stride[c];
    v.h = f.h[c] / 2;
  }
  v.w = f.w[c];
  return v;
}

// Prediction of a w x h block at integer (x, y) with half-sample flags (7.6.4), written to dst
// or averaged with what dst holds. Samples outside the reference repeat its edge.
template <int HX, int HY, bool AVG>
void predict_rows(u8* dst, int dstride, const u8* s, int sstride, int w, int h) {
  for (int j = 0; j < h; ++j, dst += dstride, s += sstride) {
    const u8* s2 = s + sstride;
    for (int i = 0; i < w; ++i) {
      int v;
      if (!HX && !HY) v = s[i];
      else if (HX && !HY) v = (s[i] + s[i + 1] + 1) >> 1;
      else if (!HX && HY) v = (s[i] + s2[i] + 1) >> 1;
      else v = (s[i] + s[i + 1] + s2[i] + s2[i + 1] + 2) >> 2;
      dst[i] = AVG ? u8((dst[i] + v + 1) >> 1) : u8(v);
    }
  }
}
template <bool AVG>
void predict_any(u8* dst, int dstride, const u8* s, int sstride, int hx, int hy, int w, int h) {
  switch (hx | hy << 1) {
    case 0: predict_rows<0, 0, AVG>(dst, dstride, s, sstride, w, h); break;
    case 1: predict_rows<1, 0, AVG>(dst, dstride, s, sstride, w, h); break;
    case 2: predict_rows<0, 1, AVG>(dst, dstride, s, sstride, w, h); break;
    default: predict_rows<1, 1, AVG>(dst, dstride, s, sstride, w, h); break;
  }
}
void predict_block(u8* dst, int dstride, const Plane& ref, int x, int y, int hx, int hy, int w, int h, bool avg) {
  if (x >= 0 && y >= 0 && x + w + hx <= ref.w && y + h + hy <= ref.h) {
    const u8* s = ref.p + y * ref.stride + x;
    if (avg) predict_any<true>(dst, dstride, s, ref.stride, hx, hy, w, h);
    else predict_any<false>(dst, dstride, s, ref.stride, hx, hy, w, h);
    return;
  }
  // Edge case: gather the (w+1) x (h+1) source area with clamped coordinates first.
  u8 tmp[17 * 17];
  for (int j = 0; j <= h; ++j)
    for (int i = 0; i <= w; ++i) {
      int xx = std::clamp(x + i, 0, ref.w - 1), yy = std::clamp(y + j, 0, ref.h - 1);
      tmp[j * 17 + i] = ref.p[yy * ref.stride + xx];
    }
  if (avg) predict_any<true>(dst, dstride, tmp, 17, hx, hy, w, h);
  else predict_any<false>(dst, dstride, tmp, 17, hx, hy, w, h);
}

// Luma and chroma prediction of a block of the macroblock from a vector in half samples.
struct MbPred {
  u8 y[256], u[64], v[64];
};
void predict(MbPred& p, const Frame& ref, int parity, int x, int y, int w, int h, int row0, int rowstep, int mvx, int mvy,
             bool avg) {
  // Luma: block of w x h at (x, y) of the reference view; rows row0, row0+rowstep, ... of p.y.
  Plane ly = view(ref, 0, parity);
  predict_block(p.y + row0 * 16, 16 * rowstep, ly, x + (mvx >> 1), y + (mvy >> 1), mvx & 1, mvy & 1, w, h, avg);
  // Chroma (4:2:0): half the vector, truncated toward zero (7.6.3.7). Interleaved field rows
  // (rowstep 2) start at the same row as luma; stacked halves start at half the luma row.
  int cx = mvx / 2, cy = mvy / 2;
  int crow0 = rowstep == 2 ? row0 : row0 / 2;
  for (int c = 1; c < 3; ++c) {
    Plane lc = view(ref, c, parity);
    u8* d = (c == 1 ? p.u : p.v) + crow0 * 8;
    predict_block(d, 8 * rowstep, lc, x / 2 + (cx >> 1), y / 2 + (cy >> 1), cx & 1, cy & 1, w / 2, h / 2, avg);
  }
}

inline int dmv_round(int x) { return (x + (x > 0)) >> 1; }  // division by 2, halves away from zero

// Parameters of the picture (or field) whose slices are being decoded; read-only meanwhile.
struct PictureParams {
  bool mpeg2 = false;
  int mb_width = 0, mb_height = 0;
  const u8* intra_matrix = nullptr;
  const u8* non_intra_matrix = nullptr;
  int coding_type = 0;
  int f_code[2][2] = {};
  bool full_pel[2] = {};
  int intra_dc_precision = 0, picture_structure = 3, top_field_first = 0, frame_pred_frame_dct = 1;
  int concealment_motion_vectors = 0, q_scale_type = 0, intra_vlc_format = 0, alternate_scan = 0;
  bool second_field = false;
  Frame* cur = nullptr;
  const Frame* ref_old = nullptr;  // older anchor: reference of P, forward reference of B
  const Frame* ref_new = nullptr;  // newer anchor: backward reference of B
};

// Decodes slices (6.2.4, 6.2.5). The slices of a picture are independent (predictors reset at
// every slice, disjoint macroblocks), so several of these run in parallel on one picture.
struct SliceDecoder : PictureParams {
  int quantiser_scale = 0;
  int dc_pred[3] = {};
  int pmv[2][2][2] = {};
  int prev_mb_type = 0, prev_motion_type = 0;
  int prev_mv[2][2][2] = {}, prev_field_select[2][2] = {};
  const char* err = nullptr;

  explicit SliceDecoder(const PictureParams& p) : PictureParams(p) {}

  bool fail(const char* m) {
    if (!err) err = m;
    return false;
  }

  // Reference for a prediction direction (7.6.2).
  const Frame* reference(int s) const {
    if (coding_type == 3) return s == 0 ? ref_old : ref_new;
    return ref_old;
  }
  // Field reference: the second field of a P picture refers to the first field of its own frame
  // when that field has the selected (opposite) parity.
  const Frame* field_reference(int s, int parity) const {
    int cur_parity = picture_structure == 1 ? 0 : 1;
    if (coding_type == 2 && second_field && parity != cur_parity) return cur;
    return reference(s);
  }

  void reset_dc() { dc_pred[0] = dc_pred[1] = dc_pred[2] = 1 << (7 + intra_dc_precision); }

  bool slice(Bits& b, int vertical_position) {
    int field_mb_height = picture_structure == 3 ? mb_height : mb_height / 2;
    int mb_row = vertical_position - 1;
    if (mb_row >= field_mb_height) return fail("slice beyond the picture");
    int qcode = int(b.get(5));
    quantiser_scale = q_scale_type ? kNonLinearScale[qcode] : qcode * 2;
    if (b.peek(1)) {  // intra_slice_flag, intra_slice, reserved, extra_bit_slice
      b.get(1);
      b.get(1);
      b.get(7);
      while (b.get(1)) b.get(8);
    } else {
      b.get(1);
    }
    reset_dc();
    std::memset(pmv, 0, sizeof pmv);
    int mb_addr = mb_row * mb_width - 1;
    bool first = true;
    for (;;) {
      // macroblock_address_increment (with escapes and MPEG-1 stuffing)
      int inc = 0;
      for (;;) {
        int v = read_vlc(b, mba_table(), 11);
        if (v == kInvalid) return fail("bad macroblock_address_increment");
        if (v == 34) {
          inc += 33;
          continue;
        }
        if (v == 35) continue;
        inc += v;
        break;
      }
      if (first) {
        mb_addr += inc;
        first = false;
      } else {
        for (int k = 1; k < inc; ++k) {  // skipped macroblocks (7.6.6)
          ++mb_addr;
          if (mb_addr >= mb_width * field_mb_height) return fail("macroblock beyond the picture");
          if (!skipped_mb(mb_addr)) return false;
        }
        ++mb_addr;
      }
      if (mb_addr >= mb_width * field_mb_height) return fail("macroblock beyond the picture");
      if (!macroblock(b, mb_addr)) return false;
      if (b.past_end()) return fail("slice data overrun");
      if (b.next_is_start_code()) return true;
    }
  }

  bool skipped_mb(int mb_addr) {
    reset_dc();
    MbPred pr;
    int mbx = mb_addr % mb_width, mby = mb_addr / mb_width;
    if (coding_type == 2) {
      std::memset(pmv, 0, sizeof pmv);
      int mv[2][2][2] = {}, fs[2][2] = {};
      if (picture_structure == 3) {
        form_prediction(pr, mbx, mby, MB_FWD, 2, mv, fs, nullptr);
      } else {
        fs[0][0] = picture_structure == 1 ? 0 : 1;  // same parity
        form_prediction(pr, mbx, mby, MB_FWD, 1, mv, fs, nullptr);
      }
    } else if (coding_type == 3) {
      form_prediction(pr, mbx, mby, prev_mb_type & (MB_FWD | MB_BWD), prev_motion_type, prev_mv, prev_field_select, nullptr);
    } else {
      return fail("skipped macroblock in an intra picture");
    }
    store_mb(pr, nullptr, nullptr, mbx, mby, false, false);
    return true;
  }

  // One motion vector component (7.6.3.1).
  int vector_component(Bits& b, int r, int s, int t, bool field_in_frame) {
    int code = read_vlc(b, motion_table(), 10);
    if (code == kInvalid) return kInvalid;
    if (code && b.get(1)) code = -code;
    int r_size = f_code[s][t] - 1;
    int f = 1 << r_size;
    int residual = 0;
    if (f != 1 && code != 0) residual = int(b.get(r_size));
    int delta;
    if (f == 1 || code == 0) {
      delta = code;
    } else {
      delta = (std::abs(code) - 1) * f + residual + 1;
      if (code < 0) delta = -delta;
    }
    int prediction = pmv[r][s][t];
    if (field_in_frame && t == 1) prediction >>= 1;
    int high = 16 * f - 1, low = -16 * f, range = 32 * f;
    int v = prediction + delta;
    if (v < low) v += range;
    if (v > high) v -= range;
    pmv[r][s][t] = field_in_frame && t == 1 ? v * 2 : v;
    return v;
  }

  // motion_vectors(s) (6.2.5.2): fills mv[r][s][t] (half samples) and field selects.
  bool motion_vectors(Bits& b, int s, int count, bool field_format, bool dual_prime, int mv[2][2][2], int fs[2][2],
                      int dmv[2]) {
    bool field_in_frame = field_format && picture_structure == 3;
    for (int r = 0; r < count; ++r) {
      if (field_format && !dual_prime) fs[r][s] = int(b.get(1));
      for (int t = 0; t < 2; ++t) {
        int v = vector_component(b, r, s, t, field_in_frame);
        if (v == kInvalid) return fail("bad motion_code");
        if (!mpeg2 && full_pel[s]) v <<= 1;
        mv[r][s][t] = v;
        if (dual_prime) {
          int d = read_vlc(b, dmv_table(), 2);
          if (d == kInvalid) return fail("bad dmvector");
          dmv[t] = d;
        }
      }
    }
    if (count == 1) {  // 7.6.3.3: both prediction vectors follow the single transmitted one
      pmv[1][s][0] = pmv[0][s][0];
      pmv[1][s][1] = pmv[0][s][1];
    }
    return true;
  }

  // Prediction for one macroblock (7.6.4). motion_type: frame pictures 1 field, 2 frame, 3 dual
  // prime; field pictures 1 field, 2 16x8, 3 dual prime.
  void form_prediction(MbPred& pr, int mbx, int mby, int dirs, int motion_type, const int mv[2][2][2], const int fs[2][2],
                       const int* dmv) {
    std::memset(pr.y, 128, sizeof pr.y);  // stays grey where a reference is missing
    std::memset(pr.u, 128, sizeof pr.u);
    std::memset(pr.v, 128, sizeof pr.v);
    bool any = false;
    for (int s = 0; s < 2; ++s) {
      if (!(dirs & (s == 0 ? MB_FWD : MB_BWD))) continue;
      bool avg = any;
      any = true;
      if (picture_structure == 3) {
        const Frame* ref = reference(s);
        if (!ref) continue;
        if (motion_type == 2) {
          predict(pr, *ref, -1, mbx * 16, mby * 16, 16, 16, 0, 1, mv[0][s][0], mv[0][s][1], avg);
        } else if (motion_type == 1) {
          for (int f = 0; f < 2; ++f)
            predict(pr, *ref, fs[f][s], mbx * 16, mby * 8, 16, 8, f, 2, mv[f][s][0], mv[f][s][1], avg);
        } else if (motion_type == 3 && dmv) {
          // Dual prime in a frame picture (7.6.3.6): each field averages its same-parity
          // prediction with the opposite-parity one from the derived vector.
          for (int f = 0; f < 2; ++f) {
            int m = f == 0 ? (top_field_first ? 1 : 3) : (top_field_first ? 3 : 1);
            int e = f == 0 ? -1 : 1;
            int dx = dmv_round(mv[0][s][0] * m) + dmv[0];
            int dy = dmv_round(mv[0][s][1] * m) + e + dmv[1];
            predict(pr, *ref, f, mbx * 16, mby * 8, 16, 8, f, 2, mv[0][s][0], mv[0][s][1], false);
            predict(pr, *ref, 1 - f, mbx * 16, mby * 8, 16, 8, f, 2, dx, dy, true);
          }
        }
      } else {
        int parity = picture_structure == 1 ? 0 : 1;
        if (motion_type == 1) {
          const Frame* ref = field_reference(s, fs[0][s]);
          if (ref) predict(pr, *ref, fs[0][s], mbx * 16, mby * 16, 16, 16, 0, 1, mv[0][s][0], mv[0][s][1], avg);
        } else if (motion_type == 2) {
          for (int h = 0; h < 2; ++h) {
            const Frame* ref = field_reference(s, fs[h][s]);
            if (ref) predict(pr, *ref, fs[h][s], mbx * 16, mby * 16 + 8 * h, 16, 8, 8 * h, 1, mv[h][s][0], mv[h][s][1], avg);
          }
        } else if (motion_type == 3 && dmv) {
          int e = parity == 0 ? -1 : 1;
          int dx = dmv_round(mv[0][s][0]) + dmv[0];
          int dy = dmv_round(mv[0][s][1]) + e + dmv[1];
          const Frame* same = field_reference(s, parity);
          const Frame* opp = field_reference(s, 1 - parity);
          if (same) predict(pr, *same, parity, mbx * 16, mby * 16, 16, 16, 0, 1, mv[0][s][0], mv[0][s][1], false);
          if (opp) predict(pr, *opp, 1 - parity, mbx * 16, mby * 16, 16, 16, 0, 1, dx, dy, true);
        }
      }
    }
  }

  // block(i) (6.2.6) with inverse scan and quantisation (7.3, 7.4): F in raster order; bit r of
  // *rows is set when row r of F holds a coefficient.
  bool block(Bits& b, int i, bool intra, int* F, u32* rows) {
    std::memset(F, 0, 64 * sizeof(int));
    const u8* scan = scans().order[alternate_scan];
    const u8* W = intra ? intra_matrix : non_intra_matrix;
    int n = 0, sum = 0;
    u32 r = 0;
    auto put = [&](int pos, int v) {
      v = std::clamp(v, -2048, 2047);
      F[pos] = v;
      sum += v;
      if (v) r |= 1u << (pos >> 3);
    };
    if (intra) {
      int cc = i < 4 ? 0 : i - 3;
      int size = read_vlc(b, dc_size_table(cc != 0), cc ? 10 : 9);
      if (size == kInvalid) return fail("bad dct_dc_size");
      int diff = 0;
      if (size) {
        int bits = int(b.get(size));
        diff = (bits & (1 << (size - 1))) ? bits : bits + 1 - (1 << size);
      }
      dc_pred[cc] += diff;
      put(0, dc_pred[cc] * (mpeg2 ? (8 >> intra_dc_precision) : 8));
      n = 1;
    }
    const auto& table = dct_table(intra && intra_vlc_format);
    bool first = !intra;
    for (;;) {
      int run, level;
      if (first && b.peek(1)) {  // first coefficient of a non-intra block: '1s' = run 0, level 1
        b.skip(1);
        run = 0;
        level = b.get(1) ? -1 : 1;
      } else {
        u32 bits16 = b.peek(16);
        const VlcEntry& e = bits16 >= 512 ? table.l1[bits16 >> 6] : table.l2[bits16];
        if (e.value == kInvalid) return fail("bad DCT coefficient code");
        b.skip(e.len);
        if (e.value == kEob) break;
        if (e.value == kEscape) {
          run = int(b.get(6));
          if (mpeg2) {
            level = int(b.get(12));
            if (level & 0x800) level -= 0x1000;
            if (level == 0 || level == -2048) return fail("bad escape level");
          } else {
            int l8 = int(b.get(8));
            if (l8 == 0) level = int(b.get(8));
            else if (l8 == 0x80) level = int(b.get(8)) - 256;
            else level = l8 >= 128 ? l8 - 256 : l8;
          }
        } else {
          run = e.value >> 8;
          level = e.value & 0xff;
          if (b.get(1)) level = -level;
        }
      }
      first = false;
      n += run;
      if (n > 63) return fail("DCT coefficients beyond the block");
      int pos = scan[n++];
      // Inverse quantisation (7.4.2); MPEG-1 makes reconstructed values odd (11172-2 2.4.4).
      int v = intra ? (level * W[pos] * quantiser_scale * 2) / 32
                    : ((2 * level + (level > 0 ? 1 : -1)) * W[pos] * quantiser_scale) / 32;
      if (!mpeg2 && v && !(v & 1)) v -= v > 0 ? 1 : -1;
      put(pos, v);
    }
    if (mpeg2 && !(sum & 1)) {  // mismatch control (7.4.4)
      F[63] ^= 1;
      if (F[63]) r |= 0x80;
    }
    *rows = r;
    return true;
  }

  // Adds the residual (if any) to the prediction and writes the macroblock (7.6.8).
  void store_mb(const MbPred& pr, const int (*res_y)[64], const int (*res_c)[64], int mbx, int mby, bool field_dct,
                bool intra) {
    Frame& f = *cur;
    int parity = picture_structure == 1 ? 0 : picture_structure == 2 ? 1 : -1;
    // Luma residual in macroblock rows; with field DCT (7.6.7), blocks 0/1 hold the top field
    // lines and blocks 2/3 the bottom field lines.
    int res[256];
    if (res_y) {
      for (int blk = 0; blk < 4; ++blk) {
        int x0 = (blk & 1) * 8;
        for (int br = 0; br < 8; ++br) {
          int row = field_dct ? 2 * br + (blk >> 1) : (blk >> 1) * 8 + br;
          std::memcpy(res + row * 16 + x0, res_y[blk] + br * 8, 8 * sizeof(int));
        }
      }
    }
    int ys = f.stride[0];
    u8* ybase = parity < 0 ? f.plane[0].data() + (mby * 16) * ys + mbx * 16
                           : f.plane[0].data() + parity * ys + (mby * 16) * 2 * ys + mbx * 16;
    int ystep = parity < 0 ? ys : 2 * ys;
    for (int row = 0; row < 16; ++row) {
      u8* d = ybase + row * ystep;
      const u8* p = pr.y + row * 16;
      const int* r = res + row * 16;
      if (intra) {
        for (int col = 0; col < 16; ++col) d[col] = clamp_pixel(r[col]);
      } else if (res_y) {
        for (int col = 0; col < 16; ++col) d[col] = clamp_pixel(p[col] + r[col]);
      } else {
        std::memcpy(d, p, 16);
      }
    }
    for (int c = 1; c < 3; ++c) {
      int cs = f.stride[c];
      u8* cbase = parity < 0 ? f.plane[c].data() + (mby * 8) * cs + mbx * 8
                             : f.plane[c].data() + parity * cs + (mby * 8) * 2 * cs + mbx * 8;
      int cstep = parity < 0 ? cs : 2 * cs;
      const u8* p = c == 1 ? pr.u : pr.v;
      const int* r = res_c ? res_c[c - 1] : nullptr;
      for (int row = 0; row < 8; ++row) {
        u8* d = cbase + row * cstep;
        if (intra) {
          for (int col = 0; col < 8; ++col) d[col] = clamp_pixel(r[row * 8 + col]);
        } else if (r) {
          for (int col = 0; col < 8; ++col) d[col] = clamp_pixel(p[row * 8 + col] + r[row * 8 + col]);
        } else {
          std::memcpy(d, p + row * 8, 8);
        }
      }
    }
  }

  bool macroblock(Bits& b, int mb_addr) {
    int mbx = mb_addr % mb_width, mby = mb_addr / mb_width;
    int type = read_vlc(b, mbtype_table(coding_type), 6);
    if (type == kInvalid) return fail("bad macroblock_type");
    int motion_type = 0;
    bool field_dct = false;
    if (type & (MB_FWD | MB_BWD)) {
      if (picture_structure == 3) motion_type = frame_pred_frame_dct ? 2 : int(b.get(2));
      else motion_type = int(b.get(2));
      if (motion_type == 0) return fail("reserved motion type");
    }
    if (picture_structure == 3 && !frame_pred_frame_dct && (type & (MB_INTRA | MB_PATTERN))) field_dct = b.get(1);
    if (type & MB_QUANT) {
      int qcode = int(b.get(5));
      quantiser_scale = q_scale_type ? kNonLinearScale[qcode] : qcode * 2;
    }
    bool intra = type & MB_INTRA;
    int mv[2][2][2] = {}, fs[2][2] = {}, dmv[2] = {};
    // Vector count and format per motion type (Tables 6-17, 6-18).
    int count = 1;
    bool field_format = picture_structure != 3, dual_prime = false;
    if (picture_structure == 3) {
      if (motion_type == 1) {
        count = 2;
        field_format = true;
      } else if (motion_type == 3) {
        field_format = true;
        dual_prime = true;
      }
    } else {
      if (motion_type == 2) count = 2;
      else if (motion_type == 3) dual_prime = true;
    }
    if ((type & MB_FWD) || (intra && concealment_motion_vectors)) {
      if (intra) {  // concealment vectors: frame format in frame pictures, field otherwise
        if (!motion_vectors(b, 0, 1, picture_structure != 3, false, mv, fs, dmv)) return false;
      } else if (!motion_vectors(b, 0, count, field_format, dual_prime, mv, fs, dmv)) {
        return false;
      }
    }
    if (type & MB_BWD)
      if (!motion_vectors(b, 1, count, field_format, false, mv, fs, dmv)) return false;
    if (intra && concealment_motion_vectors) b.get(1);  // marker
    int cbp = 0;
    if (type & MB_PATTERN) {
      cbp = read_vlc(b, cbp_table(), 9);
      if (cbp == kInvalid) return fail("bad coded_block_pattern");
    } else if (intra) {
      cbp = 63;
    }

    MbPred pr;
    if (!intra) {
      reset_dc();
      if (coding_type == 2 && !(type & MB_FWD)) {  // P macroblock without motion compensation (7.6.3.5)
        std::memset(pmv, 0, sizeof pmv);
        if (picture_structure == 3) {
          motion_type = 2;
        } else {
          motion_type = 1;
          fs[0][0] = picture_structure == 1 ? 0 : 1;
        }
        type |= MB_FWD;
        std::memset(mv, 0, sizeof mv);
      }
      form_prediction(pr, mbx, mby, type & (MB_FWD | MB_BWD), motion_type, mv, fs, dmv);
      prev_mb_type = type;
      prev_motion_type = motion_type;
      std::memcpy(prev_mv, mv, sizeof mv);
      std::memcpy(prev_field_select, fs, sizeof fs);
    } else if (!concealment_motion_vectors) {
      std::memset(pmv, 0, sizeof pmv);  // 7.6.3.4
    }

    alignas(32) int res_y[4][64], res_c[2][64];
    bool any_y = false, any_c = false;
    for (int i = 0; i < 6; ++i) {
      int* dst = i < 4 ? res_y[i] : res_c[i - 4];
      if (!(cbp & (32 >> i))) {
        std::memset(dst, 0, 64 * sizeof(int));
        continue;
      }
      alignas(32) int F[64];
      u32 rows = 0;
      if (!block(b, i, intra, F, &rows)) return false;
      idct(F, dst, rows);
      if (i < 4) any_y = true;
      else any_c = true;
    }
    store_mb(pr, (any_y || intra) ? res_y : nullptr, (any_c || intra) ? res_c : nullptr, mbx, mby, field_dct, intra);
    return true;
  }
};

// Runs fn(0..n-1) on the calling thread and a few workers; returns when all are done.
class WorkerPool {
 public:
  explicit WorkerPool(unsigned workers) {
    for (unsigned k = 0; k < workers; ++k) threads_.emplace_back([this] { loop(); });
  }
  ~WorkerPool() {
    {
      std::lock_guard lk(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  void run(size_t n, const std::function<void(size_t)>& fn) {
    {
      std::lock_guard lk(mu_);
      fn_ = &fn;
      n_ = n;
      next_ = 0;
      done_ = 0;
      ++gen_;
    }
    cv_.notify_all();
    work();
    std::unique_lock lk(mu_);
    done_cv_.wait(lk, [&] { return done_ == n_; });
    fn_ = nullptr;
  }

 private:
  void work() {
    for (;;) {
      size_t i;
      const std::function<void(size_t)>* fn;
      {
        std::lock_guard lk(mu_);
        if (!fn_ || next_ >= n_) return;
        i = next_++;
        fn = fn_;
      }
      (*fn)(i);
      std::lock_guard lk(mu_);
      if (++done_ == n_) done_cv_.notify_all();
    }
  }
  void loop() {
    u64 seen = 0;
    for (;;) {
      {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
        if (quit_) return;
        seen = gen_;
      }
      work();
    }
  }
  std::vector<std::thread> threads_;
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  const std::function<void(size_t)>* fn_ = nullptr;
  size_t n_ = 0, next_ = 0, done_ = 0;
  u64 gen_ = 0;
  bool quit_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
struct Mpeg2Decoder::Impl {
  std::string err;

  // Sequence (6.3.3, 6.3.5).
  bool have_seq = false, mpeg2 = false;
  int horizontal_size = 0, vertical_size = 0, mb_width = 0, mb_height = 0;
  int progressive_sequence = 1, low_delay = 0, chroma_format = 1;
  u8 intra_matrix[64], non_intra_matrix[64];  // raster order

  // Picture (6.3.9, 6.3.10).
  int coding_type = 0, temporal_reference = 0;
  int f_code[2][2] = {{15, 15}, {15, 15}};
  bool full_pel[2] = {false, false};
  int intra_dc_precision = 0, picture_structure = 3, top_field_first = 0, frame_pred_frame_dct = 1;
  int concealment_motion_vectors = 0, q_scale_type = 0, intra_vlc_format = 0, alternate_scan = 0;
  int repeat_first_field = 0, progressive_frame = 1;
  bool picture_pending = false;  // picture header seen, slices not started yet
  bool decoding = false;         // slices of the current picture/field are being collected
  bool skipping = false;         // current B picture is skipped

  // Frames: references in decoding order and the B picture buffer.
  Frame frames[3];
  Frame* ref_old = nullptr;
  Frame* ref_new = nullptr;
  Frame* cur = nullptr;
  int first_field_structure = 0;  // structure of the first field of `cur` (0: none pending)
  bool second_field = false;
  std::vector<Mpeg2Picture>* out = nullptr;

  // Slices of the current picture, decoded together when the picture ends.
  struct SliceRef {
    const u8* data;
    size_t size;
    int vertical_position;
  };
  std::vector<SliceRef> slices;
  std::unique_ptr<WorkerPool> pool;

  Impl() { reset_matrices(); }

  void reset_matrices() {
    std::memcpy(intra_matrix, kDefaultIntra, 64);
    std::memset(non_intra_matrix, 16, 64);
  }

  bool fail(const std::string& m) {
    if (err.empty()) err = m;
    return false;
  }

  // ---- headers ----
  void sequence_header(Bits& b) {
    int hs = int(b.get(12)), vs = int(b.get(12));
    b.get(4);   // aspect_ratio_information
    b.get(4);   // frame_rate_code
    b.get(18);  // bit_rate_value
    b.get(1);   // marker
    b.get(10);  // vbv_buffer_size_value
    b.get(1);   // constrained_parameters_flag
    const auto& zz = scans().order[0];
    if (b.get(1)) {
      for (int i = 0; i < 64; ++i) intra_matrix[zz[i]] = u8(b.get(8));
    } else {
      std::memcpy(intra_matrix, kDefaultIntra, 64);
    }
    if (b.get(1)) {
      for (int i = 0; i < 64; ++i) non_intra_matrix[zz[i]] = u8(b.get(8));
    } else {
      std::memset(non_intra_matrix, 16, 64);
    }
    horizontal_size = hs;
    vertical_size = vs;
    mpeg2 = false;  // until a sequence extension follows
    progressive_sequence = 1;
    low_delay = 0;
    chroma_format = 1;
    have_seq = true;
    update_size();
  }

  // Frame buffers for the current coded size. MPEG-1 and progressive sequences round the height to
  // macroblocks, interlaced ones to macroblock pairs.
  void update_size() {
    mb_width = (horizontal_size + 15) / 16;
    mb_height = progressive_sequence ? (vertical_size + 15) / 16 : 2 * ((vertical_size + 31) / 32);
    int w = mb_width * 16, h = mb_height * 16;
    bool changed = false;
    for (Frame& f : frames)
      if (f.w[0] != w || f.h[0] != h) {
        f.alloc(w, h);
        f.output = true;
        changed = true;
      }
    if (changed) ref_old = ref_new = nullptr;
  }

  void extension(Bits& b) {
    int id = int(b.get(4));
    if (id == 1) {  // sequence extension (6.2.2.3)
      b.get(8);     // profile_and_level_indication
      int ps = int(b.get(1));
      chroma_format = int(b.get(2));
      int hx = int(b.get(2)), vx = int(b.get(2));
      b.get(12);  // bit_rate_extension
      b.get(1);
      b.get(8);  // vbv_buffer_size_extension
      low_delay = int(b.get(1));
      mpeg2 = true;
      progressive_sequence = ps;
      horizontal_size = (horizontal_size & 0xfff) | hx << 12;
      vertical_size = (vertical_size & 0xfff) | vx << 12;
      update_size();
    } else if (id == 3) {  // quant matrix extension (6.2.3.2); chroma matrices unused for 4:2:0
      const auto& zz = scans().order[0];
      if (b.get(1))
        for (int i = 0; i < 64; ++i) intra_matrix[zz[i]] = u8(b.get(8));
      if (b.get(1))
        for (int i = 0; i < 64; ++i) non_intra_matrix[zz[i]] = u8(b.get(8));
    } else if (id == 8) {  // picture coding extension (6.2.3.1)
      for (int s = 0; s < 2; ++s)
        for (int t = 0; t < 2; ++t) f_code[s][t] = int(b.get(4));
      intra_dc_precision = int(b.get(2));
      picture_structure = int(b.get(2));
      top_field_first = int(b.get(1));
      frame_pred_frame_dct = int(b.get(1));
      concealment_motion_vectors = int(b.get(1));
      q_scale_type = int(b.get(1));
      intra_vlc_format = int(b.get(1));
      alternate_scan = int(b.get(1));
      repeat_first_field = int(b.get(1));
      b.get(1);  // chroma_420_type
      progressive_frame = int(b.get(1));
    }
  }

  void picture_header(Bits& b) {
    temporal_reference = int(b.get(10));
    coding_type = int(b.get(3));
    b.get(16);  // vbv_delay
    if (coding_type == 2 || coding_type == 3) {
      full_pel[0] = b.get(1);
      int fc = int(b.get(3));
      f_code[0][0] = f_code[0][1] = fc;
    }
    if (coding_type == 3) {
      full_pel[1] = b.get(1);
      int bc = int(b.get(3));
      f_code[1][0] = f_code[1][1] = bc;
    }
    // MPEG-1 defaults; a picture coding extension overrides them.
    intra_dc_precision = 0;
    picture_structure = 3;
    top_field_first = 0;
    frame_pred_frame_dct = 1;
    concealment_motion_vectors = 0;
    q_scale_type = 0;
    intra_vlc_format = 0;
    alternate_scan = 0;
    repeat_first_field = 0;
    progressive_frame = 1;
    if (mpeg2) full_pel[0] = full_pel[1] = false;
    picture_pending = true;
  }

  // ---- picture management (7.6.2, 6.1.1.11 reordering) ----
  void emit(Frame& f) {
    if (f.output || !out) return;
    f.output = true;
    Mpeg2Picture p;
    p.width = u32(horizontal_size);
    p.height = u32(vertical_size);
    p.tag = f.tag;
    p.coding_type = f.type;
    int w = horizontal_size, h = vertical_size, cw = (w + 1) / 2, ch = (h + 1) / 2;
    p.yuv.resize(size_t(w) * h + 2 * size_t(cw) * ch);
    u8* o = p.yuv.data();
    for (int y = 0; y < h; ++y) std::memcpy(o + size_t(y) * w, f.plane[0].data() + size_t(y) * f.stride[0], size_t(w));
    o += size_t(w) * h;
    for (int c = 1; c < 3; ++c) {
      for (int y = 0; y < ch; ++y)
        std::memcpy(o + size_t(y) * cw, f.plane[c].data() + size_t(y) * f.stride[c], size_t(std::min(cw, f.w[c])));
      o += size_t(cw) * ch;
    }
    out->push_back(std::move(p));
  }

  Frame* free_frame(const Frame* a, const Frame* b) {
    for (Frame& f : frames)
      if (&f != a && &f != b) return &f;
    return &frames[0];
  }

  // First slice of a picture or field: choose buffers.
  void begin_picture(u64 tag) {
    picture_pending = false;
    bool field = picture_structure != 3;
    second_field = field && first_field_structure != 0 && first_field_structure != picture_structure && cur;
    if (!second_field) {
      if (coding_type == 3) {
        cur = free_frame(ref_old, ref_new);
      } else {
        if (ref_new && !low_delay) emit(*ref_new);  // the previous anchor is displayed next
        ref_old = ref_new;
        cur = free_frame(ref_old, nullptr);
        ref_new = cur;
      }
      cur->tag = tag;
      cur->type = coding_type;
      cur->output = false;
      first_field_structure = field ? picture_structure : 0;
    } else {
      first_field_structure = 0;  // field pair complete after this one
    }
    decoding = true;
    slices.clear();
  }

  // Decodes the collected slices (in parallel), then completes the picture or field.
  bool end_picture() {
    decoding = false;
    bool ok = decode_slices();
    if (first_field_structure != 0) return ok;  // first field: the frame is not complete yet
    if (cur && (cur->type == 3 || low_delay)) emit(*cur);
    return ok;
  }

  bool decode_slices() {
    if (slices.empty() || !cur) return true;
    PictureParams pp;
    pp.mpeg2 = mpeg2;
    pp.mb_width = mb_width;
    pp.mb_height = mb_height;
    pp.intra_matrix = intra_matrix;
    pp.non_intra_matrix = non_intra_matrix;
    pp.coding_type = coding_type;
    std::memcpy(pp.f_code, f_code, sizeof f_code);
    pp.full_pel[0] = full_pel[0];
    pp.full_pel[1] = full_pel[1];
    pp.intra_dc_precision = intra_dc_precision;
    pp.picture_structure = picture_structure;
    pp.top_field_first = top_field_first;
    pp.frame_pred_frame_dct = frame_pred_frame_dct;
    pp.concealment_motion_vectors = concealment_motion_vectors;
    pp.q_scale_type = q_scale_type;
    pp.intra_vlc_format = intra_vlc_format;
    pp.alternate_scan = alternate_scan;
    pp.second_field = second_field;
    pp.cur = cur;
    pp.ref_old = ref_old;
    pp.ref_new = ref_new;
    if (coding_type == 2 || coding_type == 1) pp.ref_new = nullptr;  // the picture being decoded
    std::vector<const char*> errors(slices.size(), nullptr);
    auto one = [&](size_t i) {
      SliceDecoder sd(pp);
      Bits b(slices[i].data, slices[i].size);
      if (!sd.slice(b, slices[i].vertical_position)) errors[i] = sd.err ? sd.err : "slice error";
    };
    if (slices.size() >= 4) {
      if (!pool) pool = std::make_unique<WorkerPool>(worker_count());
      pool->run(slices.size(), one);
    } else {
      for (size_t i = 0; i < slices.size(); ++i) one(i);
    }
    slices.clear();
    for (const char* e : errors)
      if (e) return fail(e);
    return true;
  }

  static unsigned worker_count() {
    unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(hw / 3, 1u, 4u);
  }

  // ---- access units ----
  bool decode(const u8* data, size_t size, u64 tag, bool skip_b, std::vector<Mpeg2Picture>* o) {
    out = o;
    err.clear();
    bool ok = true;
    auto next_code = [&](size_t from) {
      for (size_t k = from; k + 3 <= size; ++k)
        if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
      return size;
    };
    size_t i = next_code(0);
    while (i + 3 < size) {
      u8 code = data[i + 3];
      size_t body = i + 4, end = next_code(body);
      size_t len = (end < size ? end : size) - body;
      if (code >= 0x01 && code <= 0xAF) {
        if (picture_pending) {
          if (!have_seq) {
            ok = fail("picture before a sequence header");
            picture_pending = false;
          } else if (coding_type == 3 && skip_b) {
            picture_pending = false;
            skipping = true;
          } else {
            skipping = false;
            begin_picture(tag);
          }
        }
        if (decoding && !skipping) slices.push_back({data + body, len, code});
      } else {
        if (decoding && !end_picture()) ok = false;
        Bits b(data + body, len);
        if (code == 0xB3) sequence_header(b);
        else if (code == 0xB5) extension(b);
        else if (code == 0x00) picture_header(b);
      }
      i = end;
    }
    if (decoding && !end_picture()) ok = false;
    out = nullptr;
    return ok;
  }

  void drain(std::vector<Mpeg2Picture>* o) {
    out = o;
    if (decoding) end_picture();
    if (ref_new && !ref_new->output) emit(*ref_new);
    out = nullptr;
  }

  void reset() {
    ref_old = ref_new = cur = nullptr;
    first_field_structure = 0;
    picture_pending = decoding = skipping = false;
    slices.clear();
    for (Frame& f : frames) f.output = true;
  }
};

Mpeg2Decoder::Mpeg2Decoder() : d_(std::make_unique<Impl>()) {}
Mpeg2Decoder::~Mpeg2Decoder() = default;
bool Mpeg2Decoder::decode(const u8* data, size_t size, u64 tag, bool skip_b, std::vector<Mpeg2Picture>* out) {
  return d_->decode(data, size, tag, skip_b, out);
}
void Mpeg2Decoder::drain(std::vector<Mpeg2Picture>* out) { d_->drain(out); }
void Mpeg2Decoder::reset() { d_->reset(); }
bool Mpeg2Decoder::have_sequence() const { return d_->have_seq; }
const std::string& Mpeg2Decoder::error() const { return d_->err; }

}  // namespace rt::media
