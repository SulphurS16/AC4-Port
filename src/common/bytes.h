// Small byte-order and integer helpers shared by tools and runtime.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

namespace ac4 {

inline u16 rd_be16(const void* p) {
  const u8* b = static_cast<const u8*>(p);
  return u16((u16(b[0]) << 8) | b[1]);
}
inline u32 rd_be32(const void* p) {
  const u8* b = static_cast<const u8*>(p);
  return (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | u32(b[3]);
}
inline u64 rd_be64(const void* p) {
  const u8* b = static_cast<const u8*>(p);
  return (u64(rd_be32(b)) << 32) | rd_be32(b + 4);
}
inline u16 rd_le16(const void* p) {
  u16 v;
  std::memcpy(&v, p, 2);
  return v;
}
inline u32 rd_le32(const void* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return v;
}
inline void wr_be16(void* p, u16 v) {
  u8* b = static_cast<u8*>(p);
  b[0] = u8(v >> 8);
  b[1] = u8(v);
}
inline void wr_be32(void* p, u32 v) {
  u8* b = static_cast<u8*>(p);
  b[0] = u8(v >> 24);
  b[1] = u8(v >> 16);
  b[2] = u8(v >> 8);
  b[3] = u8(v);
}
inline void wr_be64(void* p, u64 v) {
  wr_be32(p, u32(v >> 32));
  wr_be32(static_cast<u8*>(p) + 4, u32(v));
}

std::string hex(const void* data, size_t len);
std::vector<u8> unhex(const std::string& s);  // throws on malformed input
std::string strprintf(const char* fmt, ...);

}  // namespace ac4
