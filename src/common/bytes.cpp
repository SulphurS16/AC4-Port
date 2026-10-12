#include "common/bytes.h"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace ac4 {

std::string hex(const void* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  const u8* p = static_cast<const u8*>(data);
  std::string s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    s.push_back(digits[p[i] >> 4]);
    s.push_back(digits[p[i] & 15]);
  }
  return s;
}

std::vector<u8> unhex(const std::string& s) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string t;
  for (char c : s)
    if (c != ' ' && c != '\t') t.push_back(c);
  if (t.size() % 2) throw std::runtime_error("odd-length hex string");
  std::vector<u8> out(t.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    int hi = nib(t[2 * i]), lo = nib(t[2 * i + 1]);
    if (hi < 0 || lo < 0) throw std::runtime_error("bad hex digit");
    out[i] = u8(hi << 4 | lo);
  }
  return out;
}

std::string strprintf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string s(n > 0 ? size_t(n) : 0, '\0');
  if (n > 0) std::vsnprintf(s.data(), size_t(n) + 1, fmt, ap2);
  va_end(ap2);
  return s;
}

}  // namespace ac4
