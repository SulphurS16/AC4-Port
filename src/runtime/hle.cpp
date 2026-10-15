#include "runtime/hle.h"

#include <map>
#include <mutex>

#include "common/crypto.h"
#include "runtime/ppu_thread.h"

namespace rt::hle {

namespace {
struct Registry {
  std::map<std::pair<std::string, u32>, std::pair<std::string, Handler>> funcs;
  std::vector<Syscall> syscalls = std::vector<Syscall>(1024, Syscall{0, nullptr, nullptr});
};
Registry& reg() {
  static Registry r;
  return r;
}
}  // namespace

u32 fnid_of(const std::string& name) {
  static const u8 suffix[16] = {0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                                0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A};
  std::string s = name;
  s.append(reinterpret_cast<const char*>(suffix), 16);
  auto h = ac4::crypto::sha1(s.data(), s.size());
  return u32(h[0]) | (u32(h[1]) << 8) | (u32(h[2]) << 16) | (u32(h[3]) << 24);
}

Module::Module(const char* name, std::initializer_list<Func> funcs) {
  for (const auto& f : funcs) {
    u32 id = f.fnid ? f.fnid : fnid_of(f.name);
    reg().funcs[{name, id}] = {f.name, f.fn};
  }
}

bool find(const std::string& module, u32 fnid, Resolved& out) {
  auto it = reg().funcs.find({module, fnid});
  if (it == reg().funcs.end()) return false;
  out = {module, it->second.first, it->second.second};
  return true;
}

SyscallTable::SyscallTable(std::initializer_list<Syscall> calls) {
  for (const auto& c : calls)
    if (c.num < 1024) reg().syscalls[c.num] = c;
}

const Syscall* syscall_entry(u32 num) {
  if (num >= 1024 || !reg().syscalls[num].fn) return nullptr;
  return &reg().syscalls[num];
}

u64 call_guest_addr(ppu::Ctx& ctx, u32 code, u32 toc, std::initializer_list<u64> args) {
  u64 saved_r2 = ctx.r[2], saved_lr = ctx.lr;
  int i = 3;
  for (u64 a : args) ctx.r[i++] = a;
  ctx.r[2] = toc;
  ctx.lr = 0;  // a guest `blr` returns to us through the host call stack
  ppu::icall(ctx, code);
  ctx.r[2] = saved_r2;
  ctx.lr = saved_lr;
  return ctx.r[3];
}

u64 call_guest(ppu::Ctx& ctx, u32 opd, std::initializer_list<u64> args) {
  u32 code = ppu::r32(opd), toc = ppu::r32(opd + 4);
  return call_guest_addr(ctx, code, toc, args);
}

StackAlloc::StackAlloc(ppu::Ctx& ctx, u32 size) : ctx_(ctx), saved_sp_(ctx.r[1]) {
  // Keep the caller's frame intact: allocate below r1, then leave a fresh 0x70-byte
  // linkage area for any guest function called with the new stack pointer.
  u64 sp = (ctx.r[1] - ((size + 15) & ~15u)) & ~15ull;
  addr_ = u32(sp);
  sp -= 0x70;
  ppu::w64(u32(sp), saved_sp_);  // back chain
  ctx.r[1] = sp;
  std::memset(ppu::mem() + addr_, 0, size);
}

StackAlloc::~StackAlloc() { ctx_.r[1] = saved_sp_; }

}  // namespace rt::hle
