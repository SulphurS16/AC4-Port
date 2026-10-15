// HLE framework: imported library functions and LV2 syscalls implemented in C++.
// Arguments are marshalled from the PPC64 ABI registers (r3.., f1..) according to the C++
// signature; integer/pointer results go to r3, floating-point results to f1.
#pragma once
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <initializer_list>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "runtime/guest.h"
#include "runtime/log.h"

namespace rt::hle {

using Handler = void (*)(ppu::Ctx&);

// ---- argument marshalling ----
template <typename T>
struct is_ptr : std::false_type {};
template <typename T>
struct is_ptr<Ptr<T>> : std::true_type {};

template <typename T>
T get_arg(ppu::Ctx& ctx, int& g, int& f) {
  if constexpr (std::is_same_v<T, ppu::Ctx&>) {
    return ctx;
  } else if constexpr (is_ptr<T>::value) {
    return T(u32(ctx.r[g++]));
  } else if constexpr (std::is_floating_point_v<T>) {
    return T(ctx.f[f++]);
  } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
    return T(ctx.r[g++]);
  } else {
    static_assert(sizeof(T) == 0, "unsupported HLE argument type");
  }
}

template <typename R>
void set_ret(ppu::Ctx& ctx, R r) {
  if constexpr (is_ptr<R>::value) {
    ctx.r[3] = r.addr;
  } else if constexpr (std::is_floating_point_v<R>) {
    ctx.f[1] = double(r);
  } else if constexpr (std::is_signed_v<R>) {
    ctx.r[3] = u64(s64(r));
  } else {
    ctx.r[3] = u64(r);
  }
}

template <typename R, typename... A>
void invoke(ppu::Ctx& ctx, R (*fn)(A...)) {
  int g = 3, f = 1;
  std::tuple<A...> args{get_arg<A>(ctx, g, f)...};  // braced init: left-to-right
  if constexpr (std::is_void_v<R>) {
    std::apply(fn, args);
  } else {
    set_ret(ctx, std::apply(fn, args));
  }
}

template <auto F>
void thunk(ppu::Ctx& ctx) {
  invoke(ctx, F);
}

// ---- registration ----
// "ns::name" -> "name" (registration names must be the plain library symbol).
constexpr const char* hle_short_name(const char* s) {
  const char* r = s;
  for (const char* p = s; *p; ++p)
    if (*p == ':') r = p + 1;
  return r;
}
struct Func {
  const char* name;
  Handler fn;      // nullptr = known but not implemented (traps when called)
  u32 fnid = 0;    // 0 = derive from name
};

struct Module {
  Module(const char* name, std::initializer_list<Func> funcs);
};

#define HLE_FN(f) ::rt::hle::Func{::rt::hle::hle_short_name(#f), &::rt::hle::thunk<&f>, 0}
#define HLE_RAW(f) ::rt::hle::Func{::rt::hle::hle_short_name(#f), &f, 0}
#define HLE_TODO(f) ::rt::hle::Func{#f, nullptr, 0}

struct Resolved {
  std::string module, name;
  Handler fn;
};
// Finds the registered function for an import. Returns false if the module/fnid is unknown.
bool find(const std::string& module, u32 fnid, Resolved& out);
u32 fnid_of(const std::string& name);

// ---- syscalls ----
struct Syscall {
  u32 num;
  const char* name;
  Handler fn;
};
struct SyscallTable {
  SyscallTable(std::initializer_list<Syscall> calls);
};
#define SYSCALL(n, f) ::rt::hle::Syscall{n, #f, &::rt::hle::thunk<&f>}
#define SYSCALL_RAW(n, f) ::rt::hle::Syscall{n, #f, &f}
const Syscall* syscall_entry(u32 num);

// ---- guest calls from HLE ----
// Calls the guest function described by the OPD at `opd` with integer args in r3..; returns r3.
// Volatile registers are clobbered as by a normal call; r2 is restored.
u64 call_guest(ppu::Ctx& ctx, u32 opd, std::initializer_list<u64> args);
// Same, with a code address and explicit TOC.
u64 call_guest_addr(ppu::Ctx& ctx, u32 code, u32 toc, std::initializer_list<u64> args);

// Temporary guest stack allocation for HLE (e.g. out-parameters passed to callbacks).
class StackAlloc {
 public:
  StackAlloc(ppu::Ctx& ctx, u32 size);
  ~StackAlloc();
  u32 addr() const { return addr_; }

 private:
  ppu::Ctx& ctx_;
  u64 saved_sp_;
  u32 addr_;
};

// Tracing of HLE calls (enabled with --trace-hle; never in player builds).
constexpr bool g_trace = false;

}  // namespace rt::hle

// Common CELL error codes.
namespace rt {
constexpr s32 CELL_OK = 0;
constexpr s32 CELL_EAGAIN = s32(0x80010001);
constexpr s32 CELL_EINVAL = s32(0x80010002);
constexpr s32 CELL_ENOSYS = s32(0x80010003);
constexpr s32 CELL_ENOMEM = s32(0x80010004);
constexpr s32 CELL_ESRCH = s32(0x80010005);
constexpr s32 CELL_ENOENT = s32(0x80010006);
constexpr s32 CELL_ENOEXEC = s32(0x80010007);
constexpr s32 CELL_EDEADLK = s32(0x80010008);
constexpr s32 CELL_EPERM = s32(0x80010009);
constexpr s32 CELL_EBUSY = s32(0x8001000A);
constexpr s32 CELL_ETIMEDOUT = s32(0x8001000B);
constexpr s32 CELL_EABORT = s32(0x8001000C);
constexpr s32 CELL_EFAULT = s32(0x8001000D);
constexpr s32 CELL_ESTAT = s32(0x8001000F);
constexpr s32 CELL_EALIGN = s32(0x80010010);
constexpr s32 CELL_EKRESOURCE = s32(0x80010011);
constexpr s32 CELL_EISDIR = s32(0x80010012);
constexpr s32 CELL_ECANCELED = s32(0x80010013);
constexpr s32 CELL_EEXIST = s32(0x80010014);
constexpr s32 CELL_EISCONN = s32(0x80010015);
constexpr s32 CELL_ENOTCONN = s32(0x80010016);
constexpr s32 CELL_EAUTHFAIL = s32(0x80010017);
constexpr s32 CELL_ENOTMSELF = s32(0x80010018);
constexpr s32 CELL_ESYSVER = s32(0x80010019);
constexpr s32 CELL_EAUTHFATAL = s32(0x8001001A);
constexpr s32 CELL_EDOM = s32(0x8001001B);
constexpr s32 CELL_ERANGE = s32(0x8001001C);
constexpr s32 CELL_EILSEQ = s32(0x8001001D);
constexpr s32 CELL_EFPOS = s32(0x8001001E);
constexpr s32 CELL_EINTR = s32(0x8001001F);
constexpr s32 CELL_EFBIG = s32(0x80010020);
constexpr s32 CELL_EMLINK = s32(0x80010021);
constexpr s32 CELL_ENFILE = s32(0x80010022);
constexpr s32 CELL_ENOSPC = s32(0x80010023);
constexpr s32 CELL_ENOTTY = s32(0x80010024);
constexpr s32 CELL_EPIPE = s32(0x80010025);
constexpr s32 CELL_EROFS = s32(0x80010026);
constexpr s32 CELL_ESPIPE = s32(0x80010027);
constexpr s32 CELL_E2BIG = s32(0x80010028);
constexpr s32 CELL_EACCES = s32(0x80010029);
constexpr s32 CELL_EBADF = s32(0x8001002A);
constexpr s32 CELL_EIO = s32(0x8001002B);
constexpr s32 CELL_EMFILE = s32(0x8001002C);
constexpr s32 CELL_ENODEV = s32(0x8001002D);
constexpr s32 CELL_ENOTDIR = s32(0x8001002E);
constexpr s32 CELL_ENXIO = s32(0x8001002F);
constexpr s32 CELL_EXDEV = s32(0x80010030);
constexpr s32 CELL_EBADMSG = s32(0x80010031);
constexpr s32 CELL_EINPROGRESS = s32(0x80010032);
constexpr s32 CELL_EMSGSIZE = s32(0x80010033);
constexpr s32 CELL_ENAMETOOLONG = s32(0x80010034);
constexpr s32 CELL_ENOLCK = s32(0x80010035);
constexpr s32 CELL_ENOTEMPTY = s32(0x80010036);
constexpr s32 CELL_ENOTSUP = s32(0x80010037);
}  // namespace rt
