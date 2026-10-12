// HLE: cellSysutil (system parameters, video/audio output state, system callbacks, dialogs,
// save data, game data).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <deque>

#include "runtime/hle.h"
#include "runtime/sysutil.h"

namespace rt {
namespace game_data {
void cellGameDataCheckCreate2(ppu::Ctx& ctx);
void cellHddGameCheck(ppu::Ctx& ctx);
void cellSysCacheMount(ppu::Ctx& ctx);
}  // namespace game_data
namespace save_data {
void cellSaveDataAutoLoad2(ppu::Ctx& ctx);
void cellSaveDataAutoSave2(ppu::Ctx& ctx);
void cellSaveDataFixedLoad2(ppu::Ctx& ctx);
void cellSaveDataFixedSave2(ppu::Ctx& ctx);
void cellSaveDataListLoad2(ppu::Ctx& ctx);
void cellSaveDataListSave2(ppu::Ctx& ctx);
}  // namespace save_data
namespace {

// ---- system callbacks ----
struct Slot {
  u32 func = 0, userdata = 0;
};
std::mutex g_cb_mu;
Slot g_slots[4];
std::deque<std::pair<u64, u64>> g_events;  // (status, param)
std::deque<std::function<void(ppu::Ctx&)>> g_host_cbs;

s32 cellSysutilRegisterCallback(s32 slot, u32 func, u32 userdata) {
  if (slot < 0 || slot > 3) return s32(0x8002b101);  // CELL_SYSUTIL_ERROR_VALUE
  std::lock_guard lk(g_cb_mu);
  g_slots[slot] = {func, userdata};
  return CELL_OK;
}
s32 cellSysutilUnregisterCallback(s32 slot) {
  if (slot < 0 || slot > 3) return s32(0x8002b101);
  std::lock_guard lk(g_cb_mu);
  g_slots[slot] = {};
  return CELL_OK;
}
s32 cellSysutilCheckCallback(ppu::Ctx& ctx) {
  for (;;) {
    std::function<void(ppu::Ctx&)> fn;
    {
      std::lock_guard lk(g_cb_mu);
      if (g_host_cbs.empty()) break;
      fn = std::move(g_host_cbs.front());
      g_host_cbs.pop_front();
    }
    fn(ctx);
  }
  for (;;) {
    std::pair<u64, u64> ev;
    Slot slots[4];
    {
      std::lock_guard lk(g_cb_mu);
      if (g_events.empty()) break;
      ev = g_events.front();
      g_events.pop_front();
      std::copy(std::begin(g_slots), std::end(g_slots), slots);
    }
    LOG_I("sysutil", "delivering system event 0x%llx (param 0x%llx)", (unsigned long long)ev.first, (unsigned long long)ev.second);
    for (const Slot& s : slots)
      if (s.func) hle::call_guest(ctx, s.func, {ev.first, ev.second, s.userdata});
  }
  return CELL_OK;
}

// ---- system parameters (US console defaults) ----
s32 cellSysutilGetSystemParamInt(s32 id, Ptr<bes32> value) {
  s32 v;
  switch (id) {
    case 0x0111: v = 1; break;  // LANG: English (US)
    case 0x0112: v = 1; break;  // ENTER_BUTTON_ASSIGN: cross
    case 0x0114: v = 2; break;  // DATE_FORMAT: MMDDYYYY
    case 0x0115: v = 0; break;  // TIME_FORMAT: 12h
    case 0x0116: {              // TIMEZONE (minutes east of UTC)
      TIME_ZONE_INFORMATION tzi{};
      GetTimeZoneInformation(&tzi);
      v = -s32(tzi.Bias);
      break;
    }
    case 0x0117: v = 0; break;  // SUMMERTIME
    case 0x0121: v = 0; break;  // GAME_PARENTAL_LEVEL: off
    case 0x0122: v = 1; break;  // LICENSE_AREA: SCEA
    case 0x0123: v = 0; break;  // PARENTAL_LEVEL0_RESTRICT: off
    case 0x0141: v = 0; break;  // CURRENT_USER_HAS_NP_ACCOUNT: no (offline port)
    case 0x0151: v = 0; break;  // CAMERA_PLFREQ: disabled
    case 0x0152: v = 1; break;  // PAD_RUMBLE: on
    case 0x0153: v = 0; break;  // KEYBOARD_TYPE: US 101
    case 0x0154: v = 0; break;
    case 0x0155: v = 0; break;
    case 0x0156: v = 0; break;  // PAD_AUTOOFF: off
    case 0x0157: v = 0; break;  // MAGNETOMETER: off
    default:
      LOG_W("sysutil", "cellSysutilGetSystemParamInt: unknown id 0x%x", id);
      return s32(0x8002b102);  // CELL_SYSUTIL_ERROR_VALUE? (unknown id)
  }
  *value = v;
  return CELL_OK;
}
s32 cellSysutilGetSystemParamString(s32 id, u32 buf, u32 bufsize) {
  std::string s;
  switch (id) {
    case 0x0113: s = "Player"; break;  // NICKNAME
    case 0x0131: s = "Player"; break;  // CURRENT_USERNAME
    default: return s32(0x8002b102);
  }
  if (bufsize) write_cstr(buf, s, bufsize);
  return CELL_OK;
}

// ---- video out ----
struct DisplayMode {
  u8 resolutionId, scanMode, conversion, aspect, reserved[2];
  be16 refreshRates;
};
struct VideoOutState {
  u8 state, colorSpace, reserved[6];
  DisplayMode displayMode;
};
struct VideoOutResolution {
  be16 width, height;
};
struct VideoOutConfiguration {
  u8 resolutionId, format, aspect, reserved[9];
  be32 pitch;
};
u8 g_res_id = 2;  // 1280x720
s32 cellVideoOutGetState(u32 out, u32 dev, Ptr<VideoOutState> st) {
  if (out != 0) return s32(0x8002b222);  // CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT
  st->state = 0;                         // ENABLED
  st->colorSpace = 1;                    // RGB
  std::memset(st->reserved, 0, 6);
  st->displayMode.resolutionId = g_res_id;
  st->displayMode.scanMode = 1;  // progressive
  st->displayMode.conversion = 0;
  st->displayMode.aspect = 2;  // 16:9
  st->displayMode.reserved[0] = st->displayMode.reserved[1] = 0;
  st->displayMode.refreshRates = 0x4;  // 59.94 Hz
  return CELL_OK;
}
s32 cellVideoOutGetResolution(u32 id, Ptr<VideoOutResolution> r) {
  u16 w, h;
  switch (id) {
    case 1: w = 1920; h = 1080; break;
    case 2: w = 1280; h = 720; break;
    case 4: w = 720; h = 480; break;
    case 5: w = 720; h = 576; break;
    case 0xa: w = 1600; h = 1080; break;
    case 0xb: w = 1440; h = 1080; break;
    case 0xc: w = 1280; h = 1080; break;
    case 0xd: w = 960; h = 1080; break;
    default: return s32(0x8002b224);  // CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER
  }
  r->width = w;
  r->height = h;
  return CELL_OK;
}
s32 cellVideoOutConfigure(u32 out, Ptr<VideoOutConfiguration> cfg, u32 option, u32 wait) {
  if (out != 0) return s32(0x8002b222);
  LOG_I("sysutil", "cellVideoOutConfigure(res %u, format %u, aspect %u, pitch %u)", cfg->resolutionId, cfg->format, cfg->aspect,
        u32(cfg->pitch));
  sysutil::set_video_config(cfg->resolutionId, cfg->format, cfg->aspect, cfg->pitch);
  return CELL_OK;
}

// ---- audio out ----
struct SoundMode {
  u8 type, channel, fs, reserved;
  be32 layout;
};
struct AudioOutState {
  u8 state, encoder, reserved[6];
  be32 downMixer;
  SoundMode soundMode;
};
struct AudioOutConfiguration {
  u8 channel, encoder, reserved[10];
  be32 downMixer;
};
s32 cellAudioOutGetState(u32 out, u32 dev, Ptr<AudioOutState> st) {
  if (out != 0 && out != 1) return s32(0x8002b241);
  std::memset(st.get(), 0, sizeof(AudioOutState));
  st->state = 0;   // enabled
  st->encoder = 0; // LPCM
  st->downMixer = 0;
  st->soundMode.type = 0;     // LPCM
  st->soundMode.channel = 2;  // stereo host output (game downmix not needed: we mix to the device)
  st->soundMode.fs = 4;       // 48 kHz
  st->soundMode.layout = 1;   // CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH
  return CELL_OK;
}
s32 cellAudioOutGetSoundAvailability(u32 out, u32 type, u32 fs, u32 option) {
  if (out != 0 && out != 1) return s32(0x8002b241);
  // LPCM stereo at 48 kHz only (what the host mixer consumes).
  if (type == 0 && (fs & 4)) return 2;
  return 0;
}
s32 cellAudioOutConfigure(u32 out, Ptr<AudioOutConfiguration> cfg, u32 option, u32 wait) {
  LOG_I("sysutil", "cellAudioOutConfigure(out %u, channel %u, encoder %u, downmixer %u)", out, cfg->channel, cfg->encoder,
        u32(cfg->downMixer));
  return CELL_OK;
}
s32 cellAudioOutSetCopyControl(u32 out, u32 control) { return CELL_OK; }

}  // namespace

namespace sysutil {
static std::mutex g_vmu;
static VideoConfig g_video{2, 0, 2, 1280 * 4};
void set_video_config(u8 res, u8 fmt, u8 aspect, u32 pitch) {
  std::lock_guard lk(g_vmu);
  g_video = {res, fmt, aspect, pitch};
}
VideoConfig video_config() {
  std::lock_guard lk(g_vmu);
  return g_video;
}
void post_callback(std::function<void(ppu::Context&)> fn) {
  std::lock_guard lk(g_cb_mu);
  g_host_cbs.push_back(std::move(fn));
}
void post_event(u64 status, u64 param) {
  std::lock_guard lk(g_cb_mu);
  g_events.push_back({status, param});
}
}  // namespace sysutil

static hle::Module s_module("cellSysutil", {
    HLE_FN(cellAudioOutConfigure),
    HLE_FN(cellAudioOutGetSoundAvailability),
    HLE_FN(cellAudioOutGetState),
    HLE_FN(cellAudioOutSetCopyControl),
    HLE_RAW(game_data::cellGameDataCheckCreate2),
    HLE_RAW(game_data::cellHddGameCheck),
    // cellOskDialog*: osk_dialog.cpp
    HLE_RAW(save_data::cellSaveDataAutoLoad2),
    HLE_RAW(save_data::cellSaveDataAutoSave2),
    HLE_TODO(cellSaveDataDelete2),
    HLE_RAW(save_data::cellSaveDataFixedLoad2),
    HLE_RAW(save_data::cellSaveDataFixedSave2),
    HLE_RAW(save_data::cellSaveDataListLoad2),
    HLE_RAW(save_data::cellSaveDataListSave2),
    HLE_RAW(game_data::cellSysCacheMount),
    HLE_FN(cellSysutilCheckCallback),
    HLE_FN(cellSysutilGetSystemParamInt),
    HLE_FN(cellSysutilGetSystemParamString),
    HLE_FN(cellSysutilRegisterCallback),
    HLE_FN(cellSysutilUnregisterCallback),
    HLE_FN(cellVideoOutConfigure),
    HLE_FN(cellVideoOutGetResolution),
    HLE_FN(cellVideoOutGetState),
});

}  // namespace rt
