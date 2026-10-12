#include "common/sfo.h"

#include <cstring>
#include <stdexcept>

namespace ac4::sfo {

Map parse(const std::vector<u8>& d) {
  if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) throw std::runtime_error("not a PARAM.SFO");
  u32 key_table = rd_le32(&d[8]), data_table = rd_le32(&d[12]), n = rd_le32(&d[16]);
  if (20ull + 16ull * n > d.size()) throw std::runtime_error("truncated PARAM.SFO index");
  Map out;
  for (u32 i = 0; i < n; ++i) {
    const u8* e = &d[20 + 16 * i];
    u16 koff = rd_le16(e), fmt = rd_le16(e + 2);
    u32 len = rd_le32(e + 4), maxl = rd_le32(e + 8), doff = rd_le32(e + 12);
    size_t kpos = size_t(key_table) + koff, dpos = size_t(data_table) + doff;
    if (kpos >= d.size() || dpos + len > d.size()) throw std::runtime_error("PARAM.SFO entry out of range");
    std::string key(reinterpret_cast<const char*>(&d[kpos]), strnlen(reinterpret_cast<const char*>(&d[kpos]), d.size() - kpos));
    Value v;
    if (fmt == 0x0404 && len >= 4) {
      v.v = rd_le32(&d[dpos]);
    } else {
      std::string s(reinterpret_cast<const char*>(&d[dpos]), len);
      if (fmt == 0x0204)
        while (!s.empty() && s.back() == '\0') s.pop_back();
      v.v = s;
      v.max_len = maxl;
    }
    out[key] = v;
  }
  return out;
}

std::vector<u8> build(const Map& m) {
  std::vector<u8> keys, data;
  std::vector<u8> index;
  for (const auto& [k, v] : m) {
    u8 e[16] = {};
    u16 koff = u16(keys.size());
    keys.insert(keys.end(), k.begin(), k.end());
    keys.push_back(0);
    u32 doff = u32(data.size());
    if (std::holds_alternative<u32>(v.v)) {
      e[2] = 0x04;
      e[3] = 0x04;
      u32 x = std::get<u32>(v.v);
      u8 b[4];
      std::memcpy(b, &x, 4);
      data.insert(data.end(), b, b + 4);
      std::memcpy(e + 4, "\x04\0\0\0", 4);
      std::memcpy(e + 8, "\x04\0\0\0", 4);
    } else {
      const std::string& s = std::get<std::string>(v.v);
      u32 len = u32(s.size()) + 1;
      u32 maxl = std::max(len, v.max_len);
      maxl = (maxl + 3) & ~3u;
      e[2] = 0x04;
      e[3] = 0x02;  // utf8, NUL-terminated
      data.insert(data.end(), s.begin(), s.end());
      data.insert(data.end(), maxl - u32(s.size()), 0);
      std::memcpy(e + 4, &len, 4);
      std::memcpy(e + 8, &maxl, 4);
    }
    std::memcpy(e, &koff, 2);
    std::memcpy(e + 12, &doff, 4);
    index.insert(index.end(), e, e + 16);
  }
  while (keys.size() % 4) keys.push_back(0);
  u32 n = u32(m.size());
  u32 key_table = 20 + 16 * n, data_table = key_table + u32(keys.size());
  std::vector<u8> out(20);
  std::memcpy(out.data(), "\0PSF", 4);
  u32 version = 0x101;
  std::memcpy(&out[4], &version, 4);
  std::memcpy(&out[8], &key_table, 4);
  std::memcpy(&out[12], &data_table, 4);
  std::memcpy(&out[16], &n, 4);
  out.insert(out.end(), index.begin(), index.end());
  out.insert(out.end(), keys.begin(), keys.end());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::string get_string(const Map& m, const std::string& key, const std::string& def) {
  auto it = m.find(key);
  if (it == m.end() || !std::holds_alternative<std::string>(it->second.v)) return def;
  return std::get<std::string>(it->second.v);
}

u32 get_int(const Map& m, const std::string& key, u32 def) {
  auto it = m.find(key);
  if (it == m.end() || !std::holds_alternative<u32>(it->second.v)) return def;
  return std::get<u32>(it->second.v);
}

void set_string(Map& m, const std::string& key, const std::string& v, u32 max_len) {
  Value x;
  x.v = v.substr(0, max_len ? max_len - 1 : v.size());
  x.max_len = max_len;
  m[key] = x;
}

void set_int(Map& m, const std::string& key, u32 v) {
  Value x;
  x.v = v;
  m[key] = x;
}

}  // namespace ac4::sfo
