#include <stdexcept>

#include "prep/formats.h"

namespace ac4::prep {

std::map<std::string, SfoValue> parse_sfo(const std::vector<u8>& d) {
  if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) throw std::runtime_error("not a PARAM.SFO");
  u32 key_table = rd_le32(&d[8]), data_table = rd_le32(&d[12]), n = rd_le32(&d[16]);
  if (20ull + 16ull * n > d.size()) throw std::runtime_error("truncated PARAM.SFO index");
  std::map<std::string, SfoValue> out;
  for (u32 i = 0; i < n; ++i) {
    const u8* e = &d[20 + 16 * i];
    u16 koff = rd_le16(e), fmt = rd_le16(e + 2);
    u32 len = rd_le32(e + 4), doff = rd_le32(e + 12);
    size_t kpos = size_t(key_table) + koff, dpos = size_t(data_table) + doff;
    if (kpos >= d.size() || dpos + len > d.size()) throw std::runtime_error("PARAM.SFO entry out of range");
    std::string key(reinterpret_cast<const char*>(&d[kpos]), strnlen(reinterpret_cast<const char*>(&d[kpos]), d.size() - kpos));
    if (fmt == 0x0404 && len >= 4) {
      out[key] = rd_le32(&d[dpos]);
    } else {
      std::string v(reinterpret_cast<const char*>(&d[dpos]), len);
      while (!v.empty() && v.back() == '\0') v.pop_back();
      out[key] = v;
    }
  }
  return out;
}

std::string sfo_to_text(const std::map<std::string, SfoValue>& sfo) {
  std::string s;
  for (const auto& [k, v] : sfo) {
    if (std::holds_alternative<u32>(v))
      s += strprintf("  %-16s = 0x%08x\n", k.c_str(), std::get<u32>(v));
    else {
      std::string t = std::get<std::string>(v);
      for (auto& c : t)
        if (c == '\n') c = '|';
      s += strprintf("  %-16s = \"%s\"\n", k.c_str(), t.c_str());
    }
  }
  return s;
}

}  // namespace ac4::prep
