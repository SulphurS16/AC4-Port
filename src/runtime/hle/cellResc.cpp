// HLE: cellResc (resolution conversion), the four functions this title imports: Init,
// SetDisplayMode, GetBufferSize, SetBufferAddress. The title imports none of the conversion or
// flip functions (SetSrc, SetConvertAndFlip, ...), so libresc never draws or flips for it: these
// calls only configure the library and size its work buffers, which nothing then uses. Display
// output is the game's own gcm flips (cellGcmSys).
#include "runtime/hle.h"

namespace rt {
namespace {

constexpr s32 RESC_ERROR_NOT_INITIALIZED = s32(0x80210301), RESC_ERROR_REINITIALIZED = s32(0x80210302),
              RESC_ERROR_BAD_ALIGNMENT = s32(0x80210303), RESC_ERROR_BAD_ARGUMENT = s32(0x80210304),
              RESC_ERROR_BAD_COMBINATION = s32(0x80210307);
constexpr u32 MODE_720x480 = 0x1, MODE_720x576 = 0x2, MODE_1280x720 = 0x4, MODE_1920x1080 = 0x8;
constexpr u32 PAL_60_DROP = 1, PAL_60_INTERPOLATE = 2, PAL_60_FOR_HSYNC = 5;
constexpr u32 DISPLAY_VSYNC = 0, DISPLAY_HSYNC = 1;

struct InitConfig {
  be32 size, resourcePolicy, supportModes, ratioMode, palTemporalMode, interlaceMode, flipMode;
};

std::mutex g_mu;
bool g_init = false;
InitConfig g_cfg{};
u32 g_mode = 0;  // the display mode set (diagnostics)

s32 cellRescInit(Ptr<InitConfig> c) {
  std::lock_guard lk(g_mu);
  if (g_init) return RESC_ERROR_REINITIALIZED;
  if (!c || c->size > sizeof(InitConfig)) return RESC_ERROR_BAD_ARGUMENT;
  g_cfg = *c;
  g_init = true;
  LOG_I("resc", "cellRescInit(policy 0x%x, modes 0x%x, ratio %u, pal %u, interlace %u, flip %u)", u32(g_cfg.resourcePolicy),
        u32(g_cfg.supportModes), u32(g_cfg.ratioMode), u32(g_cfg.palTemporalMode), u32(g_cfg.interlaceMode), u32(g_cfg.flipMode));
  return CELL_OK;
}

s32 cellRescSetDisplayMode(u32 mode) {
  std::lock_guard lk(g_mu);
  if (!g_init) return RESC_ERROR_NOT_INITIALIZED;
  if (!mode || mode > MODE_1920x1080 || !(g_cfg.supportModes & mode)) return RESC_ERROR_BAD_ARGUMENT;
  if (mode == MODE_720x576) {
    u32 pal = g_cfg.palTemporalMode, flip = g_cfg.flipMode;
    if ((pal >= PAL_60_INTERPOLATE && pal < PAL_60_FOR_HSYNC) || pal == PAL_60_DROP)
      if (flip == DISPLAY_HSYNC) return RESC_ERROR_BAD_COMBINATION;
    if (pal == PAL_60_FOR_HSYNC && flip == DISPLAY_VSYNC) return RESC_ERROR_BAD_COMBINATION;
  }
  g_mode = mode;
  LOG_I("resc", "cellRescSetDisplayMode(0x%x)", g_mode);
  return CELL_OK;
}

// Work buffer sizes for the configured modes: destination color buffers for the largest supported
// mode (A8R8G8B8, pitch rounded to 64 bytes, 2 buffers, 6 for PAL 60 interpolation, 3 for PAL 60
// drop), a 4-vertex array and one fragment program. The exact figures of the system library are not
// known; these are generous and only ever allocated, since no conversion function is imported.
s32 cellRescGetBufferSize(Ptr<bes32> color, Ptr<bes32> vertex, Ptr<bes32> fragment) {
  std::lock_guard lk(g_mu);
  if (!g_init) return RESC_ERROR_NOT_INITIALIZED;
  u32 modes = g_cfg.supportModes, w = 0, h = 0, buffers = 2;
  if (modes & MODE_1920x1080) w = 1920, h = 1080;
  else if (modes & MODE_1280x720) w = 1280, h = 720;
  else if (modes & MODE_720x576) w = 720, h = 576;
  else if (modes & MODE_720x480) w = 720, h = 480;
  if (modes & MODE_720x576) {
    u32 pal = g_cfg.palTemporalMode;
    if (pal >= PAL_60_INTERPOLATE && pal < PAL_60_FOR_HSYNC) buffers = 6;
    else if (pal == PAL_60_DROP) buffers = 3;
    if (w * h < 720 * 576) w = 720, h = 576;
  }
  u32 pitch = (w * 4 + 63) & ~63u;
  if (color) *color = s32(pitch * ((h + 31) & ~31u) * buffers);
  if (vertex) *vertex = 4 * 4 * 4 * 4;  // 4 vertices, position + texture coordinates, x4 margin
  if (fragment) *fragment = 0x400;
  static bool logged = false;
  if (!logged) {
    logged = true;
    LOG_I("resc", "cellRescGetBufferSize: color 0x%x, vertex 0x%x, fragment 0x%x (sizes chosen by this port)",
          color ? u32(s32(*color)) : 0, vertex ? u32(s32(*vertex)) : 0, fragment ? u32(s32(*fragment)) : 0);
  }
  return CELL_OK;
}

s32 cellRescSetBufferAddress(u32 color, u32 vertex, u32 fragment) {
  std::lock_guard lk(g_mu);
  if (!g_init) return RESC_ERROR_NOT_INITIALIZED;
  if (!color || !vertex || !fragment) return RESC_ERROR_BAD_ARGUMENT;
  if ((color & 127) || (vertex & 3) || (fragment & 63)) return RESC_ERROR_BAD_ALIGNMENT;
  LOG_I("resc", "cellRescSetBufferAddress(color 0x%08x, vertex 0x%08x, fragment 0x%08x)", color, vertex, fragment);
  return CELL_OK;
}

}  // namespace

static hle::Module s_module("cellResc", {
    HLE_FN(cellRescGetBufferSize),
    HLE_FN(cellRescInit),
    HLE_FN(cellRescSetBufferAddress),
    HLE_FN(cellRescSetDisplayMode),
});

}  // namespace rt
