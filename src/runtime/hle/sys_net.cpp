// HLE: sys_net (BSD sockets). The port runs offline: the network stack initializes, but there is
// no network interface, so socket creation fails with ENETDOWN and name lookups fail. Pure helper
// functions (inet_addr/inet_ntoa) work normally.
#include "runtime/hle.h"
#include "runtime/memory.h"
#include "runtime/ppu_thread.h"

namespace rt {
namespace {

constexpr s32 SYS_NET_EBADF = 9, SYS_NET_EINVAL = 22, SYS_NET_ENETDOWN = 50;
std::mutex g_mu;
std::map<u32, u32> g_errno;  // ppu thread id -> guest errno address
bool g_logged = false;

u32 errno_addr() {
  PpuThread* t = cur_thread();
  u32 id = t ? t->id : 0;
  std::lock_guard lk(g_mu);
  auto it = g_errno.find(id);
  if (it != g_errno.end()) return it->second;
  u32 a = vm::alloc(vm::Area::Main, 16, 16);
  g_errno[id] = a;
  return a;
}
s32 fail(s32 err) {
  ppu::w32(errno_addr(), u32(err));
  return -1;
}
void offline_note(const char* fn) {
  std::lock_guard lk(g_mu);
  if (!g_logged) {
    g_logged = true;
    LOG_W("net", "%s: network unavailable in this port (offline); sockets fail with ENETDOWN", fn);
  }
}

s32 sys_net_initialize_network_ex(u32 param) {
  LOG_I("net", "sys_net_initialize_network_ex: local stack initialized (no network interface)");
  return CELL_OK;
}
s32 sys_net_finalize_network() { return CELL_OK; }
u32 _sys_net_errno_loc() { return errno_addr(); }
s32 sys_net_free_thread_context(u64 tid, s32 flags) { return CELL_OK; }
s32 sys_net_socket(s32 family, s32 type, s32 protocol) {
  offline_note("socket");
  return fail(SYS_NET_ENETDOWN);
}
s32 sys_net_socketclose(s32 s) { return fail(SYS_NET_EBADF); }
s32 sys_net_bind(s32 s, u32 addr, u32 len) { return fail(SYS_NET_EBADF); }
s32 sys_net_listen(s32 s, s32 backlog) { return fail(SYS_NET_EBADF); }
s32 sys_net_getsockname(s32 s, u32 addr, u32 len) { return fail(SYS_NET_EBADF); }
s32 sys_net_getsockopt(s32 s, s32 level, s32 opt, u32 val, u32 len) { return fail(SYS_NET_EBADF); }
s32 sys_net_setsockopt(s32 s, s32 level, s32 opt, u32 val, u32 len) { return fail(SYS_NET_EBADF); }
s32 sys_net_recvfrom(s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 alen) { return fail(SYS_NET_EBADF); }
s32 sys_net_sendto(s32 s, u32 buf, u32 len, s32 flags, u32 addr, u32 alen) { return fail(SYS_NET_EBADF); }
s32 sys_net_shutdown(s32 s, s32 how) { return fail(SYS_NET_EBADF); }
s32 sys_net_socketpoll(u32 fds, s32 nfds, s32 ms) { return fail(SYS_NET_EINVAL); }
u32 sys_net_gethostbyname(u32 name) {
  offline_note("gethostbyname");
  return 0;  // NULL: lookup failed
}
u32 sys_net_gethostbyaddr(u32 addr, s32 len, s32 type) { return 0; }
u32 sys_net_inet_addr(u32 cp) {
  std::string s = read_cstr(cp, 64);
  u32 parts[4], n = 0, v = 0;
  bool digit = false;
  for (char c : s + ".") {
    if (c >= '0' && c <= '9') {
      v = v * 10 + u32(c - '0');
      digit = true;
    } else if (c == '.' && digit && n < 4 && v < 256) {
      parts[n++] = v;
      v = 0;
      digit = false;
    } else {
      return 0xffffffffu;  // INADDR_NONE
    }
  }
  if (n != 4) return 0xffffffffu;
  return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];  // network byte order == guest BE
}
u32 sys_net_inet_ntoa(u32 in) {
  static u32 buf = 0;
  if (!buf) buf = vm::alloc(vm::Area::Main, 32, 16);
  write_cstr(buf, ac4::strprintf("%u.%u.%u.%u", in >> 24, (in >> 16) & 0xff, (in >> 8) & 0xff, in & 0xff), 32);
  return buf;
}
s32 sys_net_if_ctl(s32 code, u32 data, u32 len) { return fail(SYS_NET_ENETDOWN); }
s32 sys_net_show_ifconfig() { return CELL_OK; }
s32 sys_net_show_nameserver() { return CELL_OK; }
s32 sys_net_show_route() { return CELL_OK; }
s32 sys_net_set_udpp2p_test_param(u32 p) { return CELL_OK; }

}  // namespace

static hle::Module s_module("sys_net", {
    HLE_FN(_sys_net_errno_loc),
    HLE_FN(sys_net_finalize_network),
    HLE_FN(sys_net_free_thread_context),
    HLE_FN(sys_net_if_ctl),
    HLE_FN(sys_net_initialize_network_ex),
    HLE_FN(sys_net_set_udpp2p_test_param),
    HLE_FN(sys_net_show_ifconfig),
    HLE_FN(sys_net_show_nameserver),
    HLE_FN(sys_net_show_route),
    ::rt::hle::Func{"sys_net_socketpoll", &::rt::hle::thunk<&sys_net_socketpoll>, 0x051ee3eeu},
    ::rt::hle::Func{"sys_net_getsockname", &::rt::hle::thunk<&sys_net_getsockname>, 0x13efe7f5u},
    ::rt::hle::Func{"sys_net_recvfrom", &::rt::hle::thunk<&sys_net_recvfrom>, 0x1f953b9fu},
    ::rt::hle::Func{"sys_net_listen", &::rt::hle::thunk<&sys_net_listen>, 0x28e208bbu},
    ::rt::hle::Func{"sys_net_getsockopt", &::rt::hle::thunk<&sys_net_getsockopt>, 0x5a045bd1u},
    ::rt::hle::Func{"sys_net_socketclose", &::rt::hle::thunk<&sys_net_socketclose>, 0x6db6e8cdu},
    ::rt::hle::Func{"sys_net_gethostbyname", &::rt::hle::thunk<&sys_net_gethostbyname>, 0x71f4c717u},
    ::rt::hle::Func{"sys_net_inet_ntoa", &::rt::hle::thunk<&sys_net_inet_ntoa>, 0x858a930bu},
    ::rt::hle::Func{"sys_net_setsockopt", &::rt::hle::thunk<&sys_net_setsockopt>, 0x88f03575u},
    ::rt::hle::Func{"sys_net_sendto", &::rt::hle::thunk<&sys_net_sendto>, 0x9647570bu},
    ::rt::hle::Func{"sys_net_socket", &::rt::hle::thunk<&sys_net_socket>, 0x9c056962u},
    ::rt::hle::Func{"sys_net_shutdown", &::rt::hle::thunk<&sys_net_shutdown>, 0xa50777c6u},
    ::rt::hle::Func{"sys_net_bind", &::rt::hle::thunk<&sys_net_bind>, 0xb0a59804u},
    ::rt::hle::Func{"sys_net_inet_addr", &::rt::hle::thunk<&sys_net_inet_addr>, 0xdabbc2c0u},
    ::rt::hle::Func{"sys_net_gethostbyaddr", &::rt::hle::thunk<&sys_net_gethostbyaddr>, 0xf7ac8941u},
});

}  // namespace rt
