#include "runtime/guest_printf.h"

#include <cstdio>
#include <cstring>

#include "ppu/ppu_recomp.h"
#include "runtime/guest.h"

namespace rt {

u64 RegArgs::next() {
  int s = slot_++;
  if (s < 8) return ctx_.r[3 + s];
  return ppu::r64(u32(ctx_.r[1]) + 0x30 + 8 * u32(s));
}

u64 VaListArgs::next() {
  u64 v = ppu::r64(p_);
  p_ += 8;
  return v;
}

std::string guest_format(const std::string& fmt, ArgSource& args) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    char c = fmt[i];
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
      out.push_back('%');
      ++i;
      continue;
    }
    // %[flags][width][.precision][length]conv
    std::string spec = "%";
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec.push_back(fmt[j++]);
    if (j < fmt.size() && fmt[j] == '*') {
      spec += std::to_string(s32(args.next()));
      ++j;
    } else
      while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec.push_back(fmt[j++]);
    if (j < fmt.size() && fmt[j] == '.') {
      spec.push_back(fmt[j++]);
      if (j < fmt.size() && fmt[j] == '*') {
        spec += std::to_string(s32(args.next()));
        ++j;
      } else
        while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec.push_back(fmt[j++]);
    }
    int lng = 0;  // 0 int, 1 long/ll (64-bit), -1 short, -2 char
    while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j])) {
      char m = fmt[j++];
      if (m == 'h') lng = lng == -1 ? -2 : -1;
      else lng = 1;
    }
    if (j >= fmt.size()) break;
    char conv = fmt[j];
    i = j;
    char buf[512];
    u64 v;
    switch (conv) {
      case 'd':
      case 'i': {
        v = args.next();
        s64 sv = lng == 1 ? s64(v) : lng == -1 ? s64(s16(v)) : lng == -2 ? s64(s8(v)) : s64(s32(v));
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)sv);
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        v = args.next();
        u64 uv = lng == 1 ? v : lng == -1 ? u64(u16(v)) : lng == -2 ? u64(u8(v)) : u64(u32(v));
        std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)uv);
        break;
      }
      case 'p':
        std::snprintf(buf, sizeof buf, "0x%x", u32(args.next()));
        break;
      case 'c':
        std::snprintf(buf, sizeof buf, (spec + "c").c_str(), int(u8(args.next())));
        break;
      case 's': {
        u32 p = u32(args.next());
        std::string s = p ? read_cstr(p, 2048) : std::string("(null)");
        std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
        break;
      }
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      case 'a':
      case 'A': {
        double dv = ppu::bitsd(args.next());
        std::snprintf(buf, sizeof buf, (spec + conv).c_str(), dv);
        break;
      }
      case 'n':
        args.next();
        buf[0] = 0;
        break;
      default:
        std::snprintf(buf, sizeof buf, "%%%c", conv);
        break;
    }
    out += buf;
  }
  return out;
}

}  // namespace rt
