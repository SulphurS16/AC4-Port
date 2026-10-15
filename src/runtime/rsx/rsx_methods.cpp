// RSX method execution: synchronization methods, reports, flips and the 2D/memory transfer
// engines (NV0039 buffer copy, NV308A inline image, NV3089 scaled image). Rendering state and
// draw methods are forwarded to the renderer.
#include <cmath>

#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/lv2/lv2.h"
#include "runtime/rsx/methods.h"
#include "runtime/rsx/rsx_internal.h"

namespace rt::lv2 {
void sleep_us(u64 us);
}

namespace rt::rsx {

std::atomic<u32> g_acquire_ea{0}, g_acquire_value{0};  // diagnostics: pending semaphore acquire

namespace {

u32 sem_offset_406e = 0;

u8* gptr(u32 ea) { return ppu::mem() + ea; }

u32 swizzle_offset(u32 x, u32 y, u32 lw, u32 lh) {
  u32 off = 0, bit = 0, maxb = std::max(lw, lh);
  for (u32 i = 0; i < maxb; ++i) {
    if (i < lw) off |= ((x >> i) & 1) << bit++;
    if (i < lh) off |= ((y >> i) & 1) << bit++;
  }
  return off;
}

void semaphore_acquire(u32 ea, u32 value) {
  PhaseScope phase_scope(kPhWaits);
  u32 spins = 0;
  g_acquire_ea = ea;
  g_acquire_value = value;
  struct Clear {
    ~Clear() { g_acquire_ea = 0; }
  } clear;
  while (ppu::r32(ea) != value) {
    if (++spins < 256) _mm_pause();
    else lv2::sleep_us(spins < 10000 ? 10 : 100);
    if (spins == 2000000) LOG_W("rsx", "semaphore acquire at 0x%08x waiting for 0x%x (have 0x%x)", ea, value, ppu::r32(ea));
  }
  renderer_guest_sync();  // data released with the semaphore may be read by the next commands
}

void get_report(u32 arg) {
  u32 type = arg >> 24, offset = arg & 0xffffff;
  u32 loc = dma_location(reg(m::SET_CONTEXT_DMA_REPORT));
  u32 ea = loc == 0 ? kReportAddr + offset : offset_to_ea(offset, 1);
  if (!ea) {
    LOG_E("rsx", "GET_REPORT to invalid offset 0x%x (location %u)", offset, loc);
    return;
  }
  u32 value = renderer_report_value(type, ea);
  ppu::w64(ea, lv2::system_time_us() * 1000);  // nanoseconds
  ppu::w32(ea + 8, value);
  ppu::w32(ea + 12, 0);
}

// NV0039: linear memory copy with optional byte strides.
void buffer_copy() {
  s32 in_pitch = s32(reg(m::NV0039_PITCH_IN)), out_pitch = s32(reg(m::NV0039_PITCH_OUT));
  u32 line_len = reg(m::NV0039_LINE_LENGTH_IN), lines = reg(m::NV0039_LINE_COUNT);
  u32 fmt = reg(m::NV0039_FORMAT);
  u32 in_stride = fmt & 0xff, out_stride = (fmt >> 8) & 0xff;
  u32 in_loc = dma_location(reg(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN));
  u32 out_loc = dma_location(reg(m::NV0039_SET_CONTEXT_DMA_BUFFER_OUT));
  u32 src = offset_to_ea(reg(m::NV0039_OFFSET_IN), in_loc), dst = offset_to_ea(reg(m::NV0039_OFFSET_OUT), out_loc);
  if (!line_len || !lines) return;
  if (!src || !dst || in_stride == 0 || in_stride > 4 || out_stride == 0 || out_stride > 4) {
    LOG_E("rsx", "NV0039 copy ignored (src 0x%x dst 0x%x strides %u/%u)", src, dst, in_stride, out_stride);
    return;
  }
  if (lines == 1 && in_stride == 1 && out_stride == 1) {
    std::memmove(gptr(dst), gptr(src), line_len);
  } else {
    for (u32 y = 0; y < lines; ++y) {
      u8* d = gptr(dst + u32(s32(y) * out_pitch));
      const u8* s = gptr(src + u32(s32(y) * in_pitch));
      if (in_stride == 1 && out_stride == 1) std::memmove(d, s, line_len);
      else
        for (u32 x = 0; x < line_len; ++x) d[x * out_stride] = s[x * in_stride];
    }
  }
  u32 span = lines == 1 ? line_len * out_stride : u32(std::abs(out_pitch)) * (lines - 1) + line_len * out_stride;
  renderer_memory_written(out_pitch < 0 && lines > 1 ? dst + u32(s32(lines - 1) * out_pitch) : dst, span);
  bump(g.transfers);
}

// NV308A: inline image words written to the NV3062 destination surface.
void inline_color(u32 index, u32 arg) {
  u32 out_w = reg(m::NV308A_SIZE_OUT) & 0xffff;
  if (index >= out_w) return;
  u32 point = reg(m::NV308A_POINT);
  u32 x = (point & 0xffff) + index, y = point >> 16;
  u32 loc = dma_location(reg(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN));
  u32 pitch = reg(m::NV3062_SET_PITCH) >> 16;
  u32 fmt = reg(m::NV3062_SET_COLOR_FORMAT);
  u32 base = reg(m::NV3062_SET_OFFSET_DESTIN);
  if (fmt == 4) {  // R5G6B5: input treated as ARGB8
    u32 ea = offset_to_ea(base + x * 2 + y * pitch, loc);
    if (!ea) return;
    u32 r = (arg >> 16) & 0xff, gg = (arg >> 8) & 0xff, b = arg & 0xff;
    ppu::w16(ea, u16(((r * 32 / 255) << 11) | ((gg * 64 / 255) << 5) | (b * 32 / 255)));
    renderer_memory_written(ea, 2);
  } else {  // A8R8G8B8 / Y32: raw 32-bit copy
    u32 ea = offset_to_ea(base + x * 4 + y * pitch, loc);
    if (!ea) return;
    ppu::w32(ea, arg);
    renderer_memory_written(ea, 4);
  }
}

// NV3089: scaled image blit (nearest sampling) into an NV3062 or NV309E (swizzled) surface.
u32 read_pixel(const u8* p, u32 fmt) {
  switch (fmt) {
    case 7: {  // R5G6B5 -> A8R8G8B8
      u16 v = u16((p[0] << 8) | p[1]);
      u32 r = (v >> 11) & 31, gg = (v >> 5) & 63, b = v & 31;
      return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((b << 3) | (b >> 2));
    }
    case 4:  // X8R8G8B8
      return 0xff000000u | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
    default:  // A8R8G8B8
      return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
  }
}

void scaled_image() {
  u32 src_fmt = reg(m::NV3089_SET_COLOR_FORMAT);
  u32 in_size = reg(m::NV3089_IMAGE_IN_SIZE), in_fmt = reg(m::NV3089_IMAGE_IN_FORMAT);
  u32 in_w = in_size & 0xffff, in_h = in_size >> 16, in_pitch = in_fmt & 0xffff;
  u32 in_bpp = src_fmt == 7 ? 2 : 4;
  if (!in_pitch) in_pitch = in_w * in_bpp;
  u32 src_loc = dma_location(reg(m::NV3089_SET_CONTEXT_DMA_IMAGE));
  u32 src = offset_to_ea(reg(m::NV3089_IMAGE_IN_OFFSET), src_loc);
  u32 out_point = reg(m::NV3089_IMAGE_OUT_POINT), out_size = reg(m::NV3089_IMAGE_OUT_SIZE);
  u32 clip_point = reg(m::NV3089_CLIP_POINT), clip_size = reg(m::NV3089_CLIP_SIZE);
  s32 out_x = s16(out_point & 0xffff), out_y = s16(out_point >> 16);
  u32 out_w = out_size & 0xffff, out_h = out_size >> 16;
  s32 clip_x = s16(clip_point & 0xffff), clip_y = s16(clip_point >> 16);
  u32 clip_w = clip_size & 0xffff, clip_h = clip_size >> 16;
  auto fxp = [](u32 v) { return double(s32(v)) / double(1 << 20); };
  double dsdx = fxp(reg(m::NV3089_DS_DX)), dtdy = fxp(reg(m::NV3089_DT_DY));
  u32 image_in = reg(m::NV3089_IMAGE_IN);
  double in_x = double(image_in & 0xffff) / 16.0, in_y = double(image_in >> 16) / 16.0;
  bool center = ((in_fmt >> 16) & 0xff) == 1;
  if (center) {
    in_x = std::max(in_x - 0.5, 0.0);
    in_y = std::max(in_y - 0.5, 0.0);
  }
  if (!src || !in_w || !in_h || dsdx == 0 || dtdy == 0) {
    LOG_E("rsx", "NV3089 blit ignored (src 0x%x, in %ux%u, dsdx %f dtdy %f)", src, in_w, in_h, dsdx, dtdy);
    return;
  }
  bool swizzled = reg(m::NV3089_SET_CONTEXT_SURFACE) == 0x31337A73;
  u32 dst_fmt, dst_pitch = 0, dst_base, dst_loc, lw = 0, lh = 0;
  if (swizzled) {
    u32 f = reg(m::NV309E_SET_FORMAT);
    dst_fmt = f & 0xffff;
    lw = (f >> 16) & 0xff;
    lh = (f >> 24) & 0xff;
    dst_base = reg(m::NV309E_SET_OFFSET);
    dst_loc = dma_location(reg(m::NV309E_SET_CONTEXT_DMA_IMAGE));
  } else {
    dst_fmt = reg(m::NV3062_SET_COLOR_FORMAT);
    dst_pitch = reg(m::NV3062_SET_PITCH) >> 16;
    dst_base = reg(m::NV3062_SET_OFFSET_DESTIN);
    dst_loc = dma_location(reg(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN));
  }
  u32 out_bpp = dst_fmt == 4 ? 2 : 4;
  if (!dst_pitch) dst_pitch = out_w * out_bpp;
  u32 dst = offset_to_ea(dst_base, dst_loc);
  if (!dst) {
    LOG_E("rsx", "NV3089 blit to unmapped destination 0x%x", dst_base);
    return;
  }
  // Destination rectangle = output rectangle intersected with the clip rectangle.
  s32 x0 = std::max(out_x, clip_x), y0 = std::max(out_y, clip_y);
  s32 x1 = std::min<s32>(out_x + s32(out_w), clip_x + s32(clip_w)), y1 = std::min<s32>(out_y + s32(out_h), clip_y + s32(clip_h));
  u32 lo = ~0u, hi = 0;
  for (s32 y = y0; y < y1; ++y) {
    double sy = in_y + (double(y - out_y) + 0.5) * dtdy - (center ? 0.0 : 0.5);
    u32 iy = u32(std::clamp(sy, 0.0, double(in_h - 1)));
    for (s32 x = x0; x < x1; ++x) {
      double sx = in_x + (double(x - out_x) + 0.5) * dsdx - (center ? 0.0 : 0.5);
      u32 ix = u32(std::clamp(sx, 0.0, double(in_w - 1)));
      u32 px = read_pixel(gptr(src + iy * in_pitch + ix * in_bpp), src_fmt);
      u32 off = swizzled ? swizzle_offset(u32(x), u32(y), lw, lh) * out_bpp : u32(y) * dst_pitch + u32(x) * out_bpp;
      if (out_bpp == 2) {
        u32 r = (px >> 19) & 31, gg = (px >> 10) & 63, b = (px >> 3) & 31;
        ppu::w16(dst + off, u16((r << 11) | (gg << 5) | b));
      } else {
        ppu::w32(dst + off, px);
      }
      lo = std::min(lo, off);
      hi = std::max(hi, off + out_bpp);
    }
  }
  if (hi > lo) renderer_memory_written(dst + lo, hi - lo);
  bump(g.transfers);
}

}  // namespace

u32 g_method_ring[32][2];       // the last methods (dump); written by the FIFO thread only
std::atomic<u32> g_method_pos{0};  // published with plain stores: no locked operation per method
u8 g_method_special[0x10000 / 4];

void init_method_kinds() {
  auto mark = [](u32 method, u32 bytes) {
    for (u32 m = method; m < method + bytes && m < 0x10000; m += 4) g_method_special[m >> 2] = 1;
  };
  mark(m::NV308A_COLOR, 0x700 * 4);
  for (u32 m : {m::SET_REFERENCE, m::SEMAPHORE_OFFSET_406E, m::SEMAPHORE_ACQUIRE, m::SEMAPHORE_RELEASE,
                m::BACK_END_WRITE_SEMAPHORE_RELEASE, m::TEXTURE_READ_SEMAPHORE_RELEASE, m::GET_REPORT, m::NV0039_BUFFER_NOTIFY,
                m::NV3089_IMAGE_IN, m::GCM_FLIP_COMMAND})
    mark(m, 4);
  renderer_mark_special(mark);
}

void execute_method(u32 ri, u32 arg, u32 count, u32 index) {
  u32 method = ri << 2;
  {
    u32 n = g_method_pos.load(std::memory_order_relaxed);
    record_method(method, arg);
    static const u32 trace_limit = cfg().trace_rsx;
    static const double trace_at = cfg().trace_rsx_at;
    static u32 traced = 0;
    if (traced < trace_limit && (trace_at <= 0 || double(lv2::system_time_us()) / 1e6 >= trace_at)) {
      ++traced;
      LOG_I("rsx-trace", "#%u method 0x%04x = 0x%08x (%u/%u)", n, method, arg, index + 1, count);
    }
  }
  // Inline image data is not a register write.
  if (method >= m::NV308A_COLOR && method < m::NV308A_COLOR + 0x700 * 4) {
    inline_color((method - m::NV308A_COLOR) >> 2, arg);
    return;
  }
  g.regs[ri] = arg;
  switch (method) {
    case m::SET_REFERENCE:
      ppu::w32(kControlAddr + 8, arg);
      return;
    case m::SEMAPHORE_OFFSET_406E:
      sem_offset_406e = arg;
      return;
    case m::SEMAPHORE_ACQUIRE:
      if (sem_offset_406e >= 0x1000) fifo_desync("semaphore offset outside the label area", sem_offset_406e);
      semaphore_acquire(kLabelAddr + sem_offset_406e, arg);
      return;
    case m::SEMAPHORE_RELEASE:
      if (sem_offset_406e >= 0x1000) fifo_desync("semaphore offset outside the label area", sem_offset_406e);
      ppu::w32(kLabelAddr + sem_offset_406e, arg);
      return;
    case m::BACK_END_WRITE_SEMAPHORE_RELEASE: {
      renderer_flush();
      u32 v = (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff);
      ppu::w32(kLabelAddr + reg(m::SET_SEMAPHORE_OFFSET), v);
      return;
    }
    case m::TEXTURE_READ_SEMAPHORE_RELEASE:
      renderer_flush();
      ppu::w32(kLabelAddr + reg(m::SET_SEMAPHORE_OFFSET), arg);
      return;
    case m::GET_REPORT:
      get_report(arg);
      return;
    case m::NV0039_BUFFER_NOTIFY:
      buffer_copy();
      return;
    case m::NV3089_IMAGE_IN:
      scaled_image();
      return;
    case m::GCM_FLIP_COMMAND:
      renderer_flush();
      do_flip(arg);
      return;
    case m::NOTIFY:
    case m::WAIT_FOR_IDLE:
    case m::NO_OPERATION:
    case m::PIPE_NOP:
      return;
    default:
      break;
  }
  renderer_method(ri, arg, count, index);
}

}  // namespace rt::rsx
