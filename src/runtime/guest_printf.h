// printf-family formatting for guest code (liblv2 _sys_printf & co.).
#pragma once
#include <string>

#include "ppu/ppu_context.h"

namespace rt {

class ArgSource {
 public:
  virtual ~ArgSource() = default;
  virtual u64 next() = 0;  // next 64-bit argument slot
};

// Variadic arguments of a call: GPRs from r[first_gpr]..r10, then the caller's parameter save
// area (r1 + 0x30 + 8 * slot). Floating-point varargs occupy GPR/stack slots too (PPC64 ELFv1).
class RegArgs : public ArgSource {
 public:
  RegArgs(const ppu::Context& ctx, int first_gpr) : ctx_(ctx), slot_(first_gpr - 3) {}
  u64 next() override;

 private:
  const ppu::Context& ctx_;
  int slot_;
};

// A guest va_list: pointer to consecutive 8-byte slots.
class VaListArgs : public ArgSource {
 public:
  explicit VaListArgs(u32 p) : p_(p) {}
  u64 next() override;

 private:
  u32 p_;
};

std::string guest_format(const std::string& fmt, ArgSource& args);

}  // namespace rt
