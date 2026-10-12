// PARAM.SFO (PSF) reading and writing.
#pragma once
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/bytes.h"

namespace ac4::sfo {

struct Value {
  std::variant<std::string, u32> v;
  u32 max_len = 0;  // for strings: reserved size (0 = exact length + 1)
};
using Map = std::map<std::string, Value>;

Map parse(const std::vector<u8>& data);  // throws on malformed data
std::vector<u8> build(const Map& m);
std::string get_string(const Map& m, const std::string& key, const std::string& def = "");
u32 get_int(const Map& m, const std::string& key, u32 def = 0);
void set_string(Map& m, const std::string& key, const std::string& v, u32 max_len);
void set_int(Map& m, const std::string& key, u32 v);

}  // namespace ac4::sfo
