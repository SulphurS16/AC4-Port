// HLE: cellGcmSys (libgcm system part): RSX memory setup, command buffer context, control
// register, labels/reports, display buffers, flips, tiles and zcull regions.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/hle.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"
#include "runtime/rsx/methods.h"
#include "runtime/rsx/rsx.h"

namespace rt {
namespace {

constexpr s32 CELL_GCM_ERROR_FAILURE = s32(0x802100ff);
constexpr s32 CELL_GCM_ERROR_INVALID_VALUE = s32(0x80210002);

struct ContextData {
  be32 begin, end, current, callback;
};
struct GcmConfig {
  be32 localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency;
};

std::mutex g_mu;
u32 g_io_address = 0, g_io_size = 0, g_local_size = 0;
u32 g_cmd_begin = 0, g_cmd_fragments = 0;
u32 g_context_opd = 0;

struct Tile {
  u8 location, comp, bank;
  u32 offset, size, pitch;
  u16 base;
  bool valid;
};
Tile g_tiles[15];

u32 offset_of(u32 ea) {
  if (ea - rsx::kLocalBase < g_local_size) return ea - rsx::kLocalBase;
  u32 io;
  return rsx::ea_to_io(ea, &io) ? io : 0xffffffffu;
}

// Context callback: the default command buffer is split in 32 KiB fragments; when one is full
// libgcm calls this to flush and continue in the next fragment (wrapping to the first).
void gcm_callback(ppu::Ctx& ctx) {
  Ptr<ContextData> c(u32(ctx.r[3]));
  u32 cur = c->current;
  auto* ctrl_put = reinterpret_cast<std::atomic<u32>*>(ppu::mem() + rsx::kControlAddr);
  auto* ctrl_get = reinterpret_cast<std::atomic<u32>*>(ppu::mem() + rsx::kControlAddr + 4);
  ctrl_put->store(__builtin_bswap32(offset_of(cur)));
  u32 range = (cur - g_cmd_begin) / (32 * 1024);
  u32 nb, ne;
  if (range >= g_cmd_fragments - 1) {
    nb = g_cmd_begin + 4096;
    ne = g_cmd_begin + 32 * 1024 - 4;
  } else {
    nb = g_cmd_begin + (range + 1) * 32 * 1024;
    ne = g_cmd_begin + (range + 2) * 32 * 1024 - 4;
  }
  ppu::w32(cur, 0x20000000u | offset_of(nb));  // old-style jump
  c->begin = nb;
  c->current = nb;
  c->end = ne;
  // Wait until the RSX is outside the new fragment.
  u32 lo = offset_of(nb), hi = offset_of(ne);
  for (u32 spins = 0;; ++spins) {
    u32 get = __builtin_bswap32(ctrl_get->load());
    if (get < lo || get > hi) break;
    if (spins > 64) Sleep(0);
  }
  ctx.r[3] = CELL_OK;
}

s32 _cellGcmInitBody(Ptr<be32> context, u32 cmd_size, u32 io_size, u32 io_address) {
  std::lock_guard lk(g_mu);
  LOG_I("gcm", "_cellGcmInitBody(cmdSize 0x%x, ioSize 0x%x, ioAddress 0x%08x)", cmd_size, io_size, io_address);
  if ((io_address & 0xfffff) || (io_size & 0xfffff) || !io_size || cmd_size > io_size) return CELL_GCM_ERROR_FAILURE;
  g_local_size = 0x0F900000;  // SDK >= 2.2: 249 MiB usable local memory
  rsx::init(io_address, io_size, g_local_size);
  if (!rsx::map_io(io_address, 0, io_size)) return CELL_GCM_ERROR_FAILURE;
  g_io_address = io_address;
  g_io_size = io_size;
  g_cmd_begin = io_address;
  g_cmd_fragments = std::max<u32>(1, cmd_size / (32 * 1024));
  if (!g_context_opd) g_context_opd = hle_internal_opd("cellGcmCallback", gcm_callback);
  Ptr<ContextData> c(rsx::kContextAddr);
  c->begin = io_address + 4096;
  c->end = io_address + 32 * 1024 - 4;
  c->current = io_address + 4096;
  c->callback = g_context_opd;
  *context = rsx::kContextAddr;
  // Control register: RSX starts at the context begin offset.
  ppu::w32(rsx::kControlAddr, 0x1000);
  ppu::w32(rsx::kControlAddr + 4, 0x1000);
  ppu::w32(rsx::kControlAddr + 8, 0xffffffff);
  rsx::start();
  return CELL_OK;
}

void _cellGcmFunc15(u32 context) { LOG_D("gcm", "_cellGcmFunc15(0x%x) (no-op)", context); }

void cellGcmGetConfiguration(Ptr<GcmConfig> cfg) {
  cfg->localAddress = rsx::kLocalBase;
  cfg->ioAddress = g_io_address;
  cfg->localSize = g_local_size;
  cfg->ioSize = g_io_size;
  cfg->memoryFrequency = 650000000;
  cfg->coreFrequency = 500000000;
}
u32 cellGcmGetControlRegister() { return rsx::kControlAddr; }
u32 cellGcmGetLabelAddress(u8 index) { return rsx::kLabelAddr + 0x10 * index; }
u32 cellGcmGetReport(u32 type, u32 index) {
  if (type < 1 || type > 5) return 0xffffffffu;
  return ppu::r32(rsx::kReportAddr + 16 * (index & 2047) + 8);
}
s32 cellGcmAddressToOffset(u32 address, Ptr<be32> offset) {
  u32 o = offset_of(address);
  if (o == 0xffffffffu) return CELL_GCM_ERROR_FAILURE;
  *offset = o;
  return CELL_OK;
}
u32 cellGcmGetTiledPitchSize(u32 size) {
  static const u32 pitches[] = {0x0000, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0A00, 0x0C00, 0x0D00,
                                0x0E00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
                                0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000};
  for (size_t i = 0; i + 1 < std::size(pitches); ++i)
    if (pitches[i] < size && size <= pitches[i + 1]) return pitches[i + 1];
  return 0;
}
void cellGcmSetFlipMode(u32 mode) {
  LOG_I("gcm", "cellGcmSetFlipMode(%u)", mode);
  rsx::set_flip_mode(mode);
}
u64 cellGcmGetLastFlipTime() { return rsx::last_flip_time(); }
u32 cellGcmGetFlipStatus() { return rsx::flip_status(); }
void cellGcmResetFlipStatus() { rsx::reset_flip_status(); }
void cellGcmSetFlipHandler(u32 h) {
  LOG_I("gcm", "cellGcmSetFlipHandler(0x%x)", h);
  rsx::set_flip_handler(h);
}
void cellGcmSetVBlankHandler(u32 h) {
  LOG_I("gcm", "cellGcmSetVBlankHandler(0x%x)", h);
  rsx::set_vblank_handler(h);
}
void cellGcmSetGraphicsHandler(u32 h) {
  LOG_I("gcm", "cellGcmSetGraphicsHandler(0x%x) (graphics exceptions are never raised)", h);
}
s32 cellGcmSetDisplayBuffer(u8 id, u32 offset, u32 pitch, u32 width, u32 height) {
  if (id > 7) return CELL_GCM_ERROR_FAILURE;
  LOG_I("gcm", "cellGcmSetDisplayBuffer(%u, offset 0x%x, pitch %u, %ux%u)", id, offset, pitch, width, height);
  rsx::set_display_buffer(id, {offset, pitch, width, height});
  return CELL_OK;
}
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base, u8 bank) {
  if (index >= 15 || location > 1 || bank > 3) return CELL_GCM_ERROR_INVALID_VALUE;
  LOG_I("gcm", "cellGcmSetTile(%u, loc %u, offset 0x%x, size 0x%x, pitch %u, comp %u, base %u, bank %u)", index, location, offset,
        size, pitch, comp, base, bank);
  g_tiles[index] = {location, comp, bank, offset, size, pitch, base, true};
  return CELL_OK;
}
void cellGcmSetInvalidateTile(u8 index) {
  if (index < 15) g_tiles[index].valid = false;
}
// 12 arguments: 9-12 come from the caller's parameter save area.
void cellGcmSetZcull(ppu::Ctx& ctx) {
  u32 index = u32(ctx.r[3]), offset = u32(ctx.r[4]), width = u32(ctx.r[5]), height = u32(ctx.r[6]);
  u32 sp = u32(ctx.r[1]);
  LOG_I("gcm", "cellGcmSetZcull(%u, offset 0x%x, %ux%u, cullStart 0x%llx, zFormat %llu, aa %llu, dir %llu, fmt %llu, sFunc %u, sRef %u, sMask %u)",
        index, offset, width, height, (unsigned long long)ctx.r[7], (unsigned long long)ctx.r[8], (unsigned long long)ctx.r[9],
        (unsigned long long)ctx.r[10], u32(ppu::r64(sp + 0x70)), u32(ppu::r64(sp + 0x78)), u32(ppu::r64(sp + 0x80)),
        u32(ppu::r64(sp + 0x88)));
}
// Old-API flip: writes GCM_FLIP_COMMAND(id) at the context's current pointer.
void _cellGcmSetFlipCommand(Ptr<ContextData> c, u32 id) {
  u32 cur = c->current;
  ppu::w32(cur, (1u << 18) | rsx::m::GCM_FLIP_COMMAND);
  ppu::w32(cur + 4, id);
  c->current = cur + 8;
}

}  // namespace

static hle::Module s_module("cellGcmSys", {
    HLE_FN(_cellGcmFunc15),
    HLE_FN(_cellGcmInitBody),
    HLE_FN(_cellGcmSetFlipCommand),
    HLE_FN(cellGcmAddressToOffset),
    HLE_FN(cellGcmGetConfiguration),
    HLE_FN(cellGcmGetControlRegister),
    HLE_FN(cellGcmGetFlipStatus),
    HLE_FN(cellGcmGetLabelAddress),
    HLE_FN(cellGcmGetLastFlipTime),
    HLE_FN(cellGcmGetReport),
    HLE_FN(cellGcmGetTiledPitchSize),
    HLE_FN(cellGcmResetFlipStatus),
    HLE_FN(cellGcmSetDisplayBuffer),
    HLE_FN(cellGcmSetFlipHandler),
    HLE_FN(cellGcmSetFlipMode),
    HLE_FN(cellGcmSetGraphicsHandler),
    HLE_FN(cellGcmSetInvalidateTile),
    HLE_FN(cellGcmSetTile),
    HLE_FN(cellGcmSetVBlankHandler),
    HLE_RAW(cellGcmSetZcull),
});

}  // namespace rt
