// HLE: cellNetCtl (network connection control) and cellGameUpdate. The port has no network
// interface: the connection state is permanently "disconnected", the network start dialog
// finishes immediately with "not connected", and update checks report an error rather than a
// fabricated "no update available".
#include "runtime/hle.h"
#include "runtime/sysutil.h"

namespace rt {
namespace {

constexpr s32 CELL_NET_CTL_ERROR_NOT_INITIALIZED = s32(0x80130101);
constexpr s32 CELL_NET_CTL_ERROR_NOT_TERMINATED = s32(0x80130102);
constexpr s32 CELL_NET_CTL_ERROR_HANDLER_MAX = s32(0x80130103);
constexpr s32 CELL_NET_CTL_ERROR_ID_NOT_FOUND = s32(0x80130104);
constexpr s32 CELL_NET_CTL_ERROR_INVALID_CODE = s32(0x80130106);
constexpr s32 CELL_NET_CTL_ERROR_INVALID_ADDR = s32(0x80130107);
constexpr s32 CELL_NET_CTL_ERROR_NOT_CONNECTED = s32(0x80130108);
constexpr s32 CELL_NET_CTL_ERROR_INVALID_TYPE = s32(0x8013010a);
constexpr s32 CELL_NET_CTL_ERROR_INVALID_SIZE = s32(0x8013010b);
constexpr s32 CELL_NET_CTL_ERROR_NET_NOT_CONNECTED = s32(0x80130182);
constexpr s32 CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED = s32(0x8002cc01);
constexpr s32 CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED = s32(0x8002cc02);
constexpr s32 CELL_GAMEUPDATE_ERROR_INVALID_ADDR = s32(0x8002cc03);

constexpr u64 kNetStartLoaded = 0x0801, kNetStartFinished = 0x0802, kNetStartUnloaded = 0x0803;
constexpr s32 kGameUpdateStatusError = 3, kGameUpdateStatusFinished = 5;
constexpr int kHandlerMax = 3;

std::mutex g_mu;
bool g_init = false, g_update_init = false;
struct Handler {
  u32 func = 0, arg = 0;
};
Handler g_handlers[kHandlerMax];

s32 cellNetCtlInit() {
  std::lock_guard lk(g_mu);
  if (g_init) return CELL_NET_CTL_ERROR_NOT_TERMINATED;
  g_init = true;
  LOG_I("netctl", "cellNetCtlInit: no network interface in this port; state is Disconnected");
  return CELL_OK;
}
s32 cellNetCtlTerm() {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  g_init = false;
  for (Handler& h : g_handlers) h = {};
  return CELL_OK;
}
s32 cellNetCtlGetState(Ptr<bes32> state) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  if (!state) return CELL_NET_CTL_ERROR_INVALID_ADDR;
  *state = 0;  // CELL_NET_CTL_STATE_Disconnected
  return CELL_OK;
}
s32 cellNetCtlAddHandler(u32 func, u32 arg, Ptr<bes32> hid) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  if (!func || !hid) return CELL_NET_CTL_ERROR_INVALID_ADDR;
  for (int i = 0; i < kHandlerMax; ++i) {
    if (g_handlers[i].func) continue;
    g_handlers[i] = {func, arg};
    *hid = i;
    return CELL_OK;  // never invoked: the state never changes while offline
  }
  return CELL_NET_CTL_ERROR_HANDLER_MAX;
}
s32 cellNetCtlDelHandler(s32 hid) {
  std::lock_guard lk(g_mu);
  if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  if (hid < 0 || hid >= kHandlerMax || !g_handlers[hid].func) return CELL_NET_CTL_ERROR_ID_NOT_FOUND;
  g_handlers[hid] = {};
  return CELL_OK;
}
s32 cellNetCtlGetInfo(s32 code, Ptr<be32> info) {
  {
    std::lock_guard lk(g_mu);
    if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  }
  if (!info) return CELL_NET_CTL_ERROR_INVALID_ADDR;
  if (code < 1 || code > 26) return CELL_NET_CTL_ERROR_INVALID_CODE;
  switch (code) {
    case 1: *info = 0; return CELL_OK;  // DEVICE: wired
    case 4: *info = 0; return CELL_OK;  // LINK: disconnected
    default: return CELL_NET_CTL_ERROR_NOT_CONNECTED;
  }
}

struct NetStartDialogParam {
  be32 size, cid, type;
};
struct NetStartDialogResult {
  be32 size;
  bes32 result;
};
s32 cellNetCtlNetStartDialogLoadAsync(Ptr<NetStartDialogParam> param) {
  {
    std::lock_guard lk(g_mu);
    if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  }
  if (!param) return CELL_NET_CTL_ERROR_INVALID_ADDR;
  if (u32(param->type) >= 2) return CELL_NET_CTL_ERROR_INVALID_TYPE;
  if (u32(param->size) != 12) return CELL_NET_CTL_ERROR_INVALID_SIZE;
  LOG_W("netctl", "network start dialog (type %u): no network available, finishing immediately", u32(param->type));
  sysutil::post_event(kNetStartLoaded, 0);
  sysutil::post_event(kNetStartFinished, 0);
  return CELL_OK;
}
s32 cellNetCtlNetStartDialogUnloadAsync(Ptr<NetStartDialogResult> result) {
  {
    std::lock_guard lk(g_mu);
    if (!g_init) return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
  }
  if (!result) return CELL_NET_CTL_ERROR_INVALID_ADDR;
  if (u32(result->size) != 8) return CELL_NET_CTL_ERROR_INVALID_SIZE;
  result->result = CELL_NET_CTL_ERROR_NET_NOT_CONNECTED;
  sysutil::post_event(kNetStartUnloaded, 0);
  return CELL_OK;
}

// ---- game update check ----
s32 cellGameUpdateInit() {
  std::lock_guard lk(g_mu);
  if (g_update_init) return CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED;
  g_update_init = true;
  return CELL_OK;
}
s32 cellGameUpdateCheckStartAsync(u32 param, u32 cb, u32 userdata) {
  {
    std::lock_guard lk(g_mu);
    if (!g_update_init) return CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
  }
  if (!param || !cb) return CELL_GAMEUPDATE_ERROR_INVALID_ADDR;
  LOG_W("netctl", "game update check: no network available; reporting an error to the game");
  sysutil::post_callback([cb, userdata](ppu::Context& ctx) {
    hle::call_guest(ctx, cb, {u64(kGameUpdateStatusError), u64(s64(CELL_NET_CTL_ERROR_NET_NOT_CONNECTED)), userdata});
  });
  return CELL_OK;
}
s32 cellGameUpdateCheckFinishAsync(u32 cb, u32 userdata) {
  {
    std::lock_guard lk(g_mu);
    if (!g_update_init) return CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
  }
  if (!cb) return CELL_GAMEUPDATE_ERROR_INVALID_ADDR;
  sysutil::post_callback([cb, userdata](ppu::Context& ctx) {
    hle::call_guest(ctx, cb, {u64(kGameUpdateStatusFinished), u64(CELL_OK), userdata});
  });
  return CELL_OK;
}

}  // namespace

static hle::Module s_module("cellNetCtl", {
    HLE_FN(cellGameUpdateCheckFinishAsync),
    HLE_FN(cellGameUpdateCheckStartAsync),
    HLE_FN(cellGameUpdateInit),
    HLE_FN(cellNetCtlAddHandler),
    HLE_FN(cellNetCtlDelHandler),
    HLE_FN(cellNetCtlGetInfo),
    HLE_FN(cellNetCtlGetState),
    HLE_FN(cellNetCtlInit),
    HLE_FN(cellNetCtlNetStartDialogLoadAsync),
    HLE_FN(cellNetCtlNetStartDialogUnloadAsync),
    HLE_FN(cellNetCtlTerm),
});

}  // namespace rt
