// HLE: AV chat (cellSysutilAvc / cellSysutilAvcExt). Voice chat needs a PSN matching room, which
// cannot exist in the offline port, so the utility cannot be loaded; option parameters and
// attributes are handled as plain local data.
#include "runtime/hle.h"

namespace rt {
namespace {

constexpr s32 CELL_AVC_ERROR_UNKNOWN = s32(0x8002b701);
constexpr s32 CELL_AVC_ERROR_NOT_SUPPORTED = s32(0x8002b702);
constexpr s32 CELL_AVC_ERROR_NOT_INITIALIZED = s32(0x8002b703);
constexpr s32 CELL_AVC_ERROR_INVALID_ARGUMENT = s32(0x8002b705);

struct AvcOptionParam {
  bes32 version;
  u8 sharing_video_buffer, pad[3];
  bes32 max_players;  // version 180 and later
};

std::mutex g_mu;
std::map<u32, u32> g_attributes;  // attribute id -> first word of the parameter
bool g_logged = false;

s32 unavailable(const char* fn) {
  std::lock_guard lk(g_mu);
  if (!g_logged) {
    g_logged = true;
    LOG_W("avc", "%s: voice chat needs PlayStation Network, unavailable in this port (offline)", fn);
  }
  return CELL_AVC_ERROR_NOT_SUPPORTED;
}

s32 cellSysutilAvcExtInitOptionParam(s32 version, Ptr<AvcOptionParam> option) {
  if (!option) return CELL_AVC_ERROR_INVALID_ARGUMENT;
  option->version = version;
  if (version == 180)
    option->max_players = 16;
  else if (version != 100)
    return CELL_AVC_ERROR_UNKNOWN;
  option->sharing_video_buffer = 0;
  return CELL_OK;
}
s32 cellSysutilAvcExtLoadAsyncEx(u32 func, u32 userdata, u32 container, u32 media, u32 video_quality,
                                 u32 voice_quality, Ptr<AvcOptionParam> option, u32 request_id) {
  if (!option) return CELL_AVC_ERROR_INVALID_ARGUMENT;
  s32 v = option->version;
  if (v != 100 && v != 180) return CELL_AVC_ERROR_UNKNOWN;
  if (option->sharing_video_buffer && media == 1) return CELL_AVC_ERROR_INVALID_ARGUMENT;  // voice chat
  if (!func || !request_id) return CELL_AVC_ERROR_INVALID_ARGUMENT;
  return unavailable("cellSysutilAvcExtLoadAsyncEx");
}
s32 cellSysutilAvcExtStartVoiceDetection() { return CELL_AVC_ERROR_NOT_INITIALIZED; }
s32 cellSysutilAvcExtStopVoiceDetection() { return CELL_AVC_ERROR_NOT_INITIALIZED; }

s32 cellSysutilAvcSetAttribute(u32 attr_id, u32 param) {
  if (!param) return CELL_AVC_ERROR_INVALID_ARGUMENT;
  std::lock_guard lk(g_mu);
  g_attributes[attr_id] = ppu::r32(param);
  return CELL_OK;
}
s32 cellSysutilAvcJoinRequest(u32 room_id, u32 request_id) { return CELL_AVC_ERROR_NOT_INITIALIZED; }
s32 cellSysutilAvcByeRequest(u32 request_id) { return CELL_AVC_ERROR_NOT_INITIALIZED; }
s32 cellSysutilAvcUnloadAsync(u32 request_id) { return CELL_AVC_ERROR_NOT_INITIALIZED; }

}  // namespace

static hle::Module s_module("cellSysutilAvcExt", {
    HLE_FN(cellSysutilAvcExtInitOptionParam),
    HLE_FN(cellSysutilAvcExtLoadAsyncEx),
    HLE_FN(cellSysutilAvcExtStartVoiceDetection),
    HLE_FN(cellSysutilAvcExtStopVoiceDetection),
});

// These are exported by the cellSysutil library on the console.
static hle::Module s_sysutil_avc("cellSysutil", {
    HLE_FN(cellSysutilAvcByeRequest),
    HLE_FN(cellSysutilAvcJoinRequest),
    HLE_FN(cellSysutilAvcSetAttribute),
    HLE_FN(cellSysutilAvcUnloadAsync),
});

}  // namespace rt
