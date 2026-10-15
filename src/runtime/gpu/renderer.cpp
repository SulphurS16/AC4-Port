// RSX renderer (stage 2): renders NV4097 draws with Direct3D 11 (rsx_facts.md §1-§8).
//
// Surfaces live on the GPU, keyed by (location, offset); a flip presents the color surface at the
// display buffer's offset, or guest memory when nothing was rendered there. Vertex programs and
// fragment programs are translated to HLSL (vp_hlsl/fp_hlsl) and cached. Vertex data is uploaded
// as raw guest bytes and decoded by the vertex shader (see "Raw vertex streams"). Known gaps are
// logged once and listed in docs/STATUS.md.
#include <d3dcompiler.h>
#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/crypto.h"
#include "common/file.h"
#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/gpu/gpu_timing.h"
#include "runtime/gpu/submit.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/fp_hlsl.h"
#include "runtime/gpu/png.h"
#include "runtime/gpu/rsx_texture.h"
#include "runtime/gpu/vp_hlsl.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/rsx/methods.h"
#include "runtime/rsx/rsx_internal.h"
#include "runtime/settings.h"

namespace rt::gpu {
std::atomic<int> g_capture_requests{0};
std::atomic<u32> g_capture_index{0};
}  // namespace rt::gpu

namespace rt::rsx {
namespace {

using gpu::device;

// Objects may still be referenced by recorded calls (submit.h): they are released after those.
template <typename T>
void release(T*& p) {
  gpu::rec().release(p);
  p = nullptr;
}

// Content hash of guest data (8 bytes per step).
u64 hash_bytes(const u8* p, size_t n) {
  u64 h = 0x9e3779b97f4a7c15ull ^ n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    u64 w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0xff51afd7ed558ccdull;
    h ^= h >> 32;
  }
  for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

u64 fnv(const void* data, size_t n, u64 h = 0xcbf29ce484222325ull) {
  const u8* p = static_cast<const u8*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}
float f32_of(u32 v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::map<std::string, int> g_warned;
void warn_once(const std::string& key, const std::string& msg) {
  if (g_warned[key]++) return;
  LOG_W("gpu", "%s", msg.c_str());
}

bool g_have_device = false;
u64 g_frame = 0;

// Work counters, logged as per-frame averages every 300 frames (perf).
struct RenderStats {
  u64 draws = 0, vcached = 0, vconverted = 0, tex_hashed = 0, tex_uploaded = 0, surfaces = 0, target_binds = 0, collections = 0,
      bytes_queried = 0, vertices = 0, max_draw_vertices = 0, vbytes = 0, query_calls = 0, query_ticks = 0, query_written = 0,
      cond_draws = 0, cond_skipped = 0, zpass_reports = 0, zcull_reports = 0;
};
RenderStats g_rs, g_rs_last;

// Phase timing (rsx_internal.h): performance counter ticks per phase.
const char* const kPhaseNames[kPhCount] = {"methods",    "surfaces", "shaders", "textures", "vertices",
                                           "write checks", "draw calls", "flip", "waits", "state",
                                           "constants",  "uploads"};
u64 g_phase_ticks[kPhCount];
double g_ms_per_tick = 0;  // TSC rate (pacing_frame)
int g_phase = kPhMethods;
u64 g_phase_at = 0;

// Pipeline state as last set by the draw path: state that did not change is not set again. Code
// that sets pipeline state directly calls shadow_reset() afterwards.
struct Shadow {
  ID3D11RenderTargetView* rtv[4];
  ID3D11DepthStencilView* dsv;
  u32 rt_count, vp_w, vp_h;
  ID3D11BlendState* blend;
  float blend_factor[4];
  ID3D11DepthStencilState* depth;
  u32 stencil_ref;
  ID3D11RasterizerState* raster;
  D3D11_RECT scissor;
  ID3D11VertexShader* vs;
  ID3D11PixelShader* ps;
  ID3D11ShaderResourceView* vs_srv[4];  // vertex streams (gpu::kVtxBuffers)
  ID3D11ShaderResourceView* ps_srv[16];
  ID3D11SamplerState* smp[16];
  ID3D11Buffer* ib;
  u32 topo;
  u32 fixed;  // nonzero: constant buffer bindings and input layout in place
};
Shadow g_sh;
void shadow_reset() {
  std::memset(&g_sh, 0xff, sizeof g_sh);  // matches no real state
  g_sh.fixed = 0;
}
// Sets the slots of want[N] that differ from have[N] (one call over the differing span).
template <u32 N = 16, typename T, typename F>
void bind_changed(T** want, T** have, F set) {
  u32 lo = N, hi = 0;
  for (u32 i = 0; i < N; ++i)
    if (want[i] != have[i]) {
      lo = std::min(lo, i);
      hi = i + 1;
    }
  if (lo >= hi) return;
  set(lo, hi - lo);
  std::copy(want + lo, want + hi, have + lo);
}

// ---------------------------------------------------------------------------------------------
// Surfaces
struct Surface {
  u32 location = 0, offset = 0, format = 0;
  u32 width = 0, height = 0;  // storage: the game's size times the render scale
  bool depth = false;
  u32 gw = 0, gh = 0;  // the game's size (antialiased surfaces: their super-sized storage)
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D* tex = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  u64 last_frame = 0;
  u64 written = 0;  // serial of the last draw/clear that targeted it
  // Copy for draws that sample the surface they render to (feedback), refreshed when written.
  ID3D11Texture2D* copy = nullptr;
  ID3D11ShaderResourceView* copy_srv = nullptr;
  u64 copy_serial = ~0ull;
  // Depth surfaces sampled with a color format: the D24S8 bits as A8R8G8B8 texels.
  ID3D11ShaderResourceView* stencil_srv = nullptr;
  ID3D11Texture2D* as_color = nullptr;
  ID3D11RenderTargetView* as_color_rtv = nullptr;
  ID3D11ShaderResourceView* as_color_srv = nullptr;
  u64 as_color_serial = ~0ull;
};
std::vector<Surface*> g_surfaces;
// Index of g_surfaces by address (one surface per location and offset): lookups per draw for the
// targets and every texture (a linear search over a few hundred surfaces was ~5% of the FIFO thread).
std::unordered_map<u64, Surface*> g_surface_at;
u64 surface_key(u32 location, u32 offset) { return (u64(location) << 32) | offset; }
u64 g_write_serial = 0;

// ---- Render scale (true resolution rendering) ----
// Surfaces are stored at the game's size times the render scale: the 16:9 picture's height in
// the window or on the screen over the console's 720 lines (2 at 1440p, 3 at 2160p), times the
// player's render scale (50-200 %). Vertex positions reach clip space through the game's own
// surface size and fragment programs see window positions in the game's pixels, so only the
// resolution changes. A new scale applies at a frame boundary, once the output size has settled;
// surfaces are re-created at their new size, color ones with their content stretched into them.
float g_scale = 1.0f;
float g_last_area = 1.0f;  // storage pixels per game sample of the last bound targets (pixel counts)
constexpr u32 kMaxStorage = 8192;  // per dimension (memory; D3D11 allows 16384)
u32 scaled(u32 v, float f) { return std::max<u32>(1, u32(std::lround(double(v) * f))); }

DXGI_FORMAT color_dxgi(u32 f) {
  switch (f) {
    case 11: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 12: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 13: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

void fr_forget(Surface* s);
void destroy(Surface* s) {
  fr_forget(s);
  release(s->rtv);
  release(s->dsv);
  release(s->srv);
  release(s->tex);
  release(s->copy_srv);
  release(s->copy);
  release(s->stencil_srv);
  release(s->as_color_srv);
  release(s->as_color_rtv);
  release(s->as_color);
  delete s;
}

Surface* find_surface(u32 location, u32 offset) {
  auto it = g_surface_at.find(surface_key(location, offset));
  return it == g_surface_at.end() ? nullptr : it->second;
}

u64 surface_bytes(const Surface* s) {
  u32 bpp = s->depth ? (s->format == 1 ? 2 : 4) : s->format == 11 ? 8 : s->format == 12 ? 16 : 4;
  return u64(s->width) * s->height * bpp * (s->copy ? 2 : 1);
}
void shadow_reset();
// Render targets of earlier scenes stay until their address is reused: once they exceed the
// budget, surfaces neither rendered to nor sampled for a minute are destroyed.
void surface_trim() {
  const u64 kBudget = std::min<u64>(u64((512ull << 20) * std::max(1.0f, g_scale * g_scale)), 3072ull << 20);
  u64 total = 0;
  for (const Surface* s : g_surfaces) total += surface_bytes(s);
  if (total <= kBudget) return;
  u32 dropped = 0;
  for (auto it = g_surfaces.begin(); it != g_surfaces.end();) {
    Surface* s = *it;
    if (s->last_frame + 3600 < g_frame) {
      total -= surface_bytes(s);
      g_surface_at.erase(surface_key(s->location, s->offset));
      it = g_surfaces.erase(it);
      destroy(s);
      ++dropped;
    } else {
      ++it;
    }
  }
  if (dropped) {
    shadow_reset();
    LOG_I("gpu", "surfaces: %u unused surface(s) destroyed, %.0f MiB remain", dropped, total / 1048576.0);
  }
}

void stretch_copy(Surface* from, Surface* to);
// gw, gh: the game's size of the surface; it is stored at that size times the render scale.
Surface* get_surface(u32 location, u32 offset, u32 format, u32 gw, u32 gh, bool depth) {
  float f = std::min({g_scale, float(kMaxStorage) / float(gw), float(kMaxStorage) / float(gh)});
  u32 w = scaled(gw, f), h = scaled(gh, f);
  Surface* s = find_surface(location, offset);
  if (s && s->depth == depth && s->format == format && s->gw == gw && s->gh == gh && s->width == w && s->height == h) {
    s->last_frame = g_frame;
    return s;
  }
  Surface* old = nullptr;  // a re-scaled color surface keeps its content
  if (s) {
    if (!depth && !s->depth && s->format == format && s->gw == gw && s->gh == gh) {
      old = s;
    } else {
      static int logged = 0;
      if (logged < 24) {
        ++logged;
        LOG_I("gpu", "surface at %s 0x%08x recreated: %s %ux%u fmt %u -> %s %ux%u fmt %u", location ? "main" : "local", offset,
              s->depth ? "depth" : "color", s->gw, s->gh, s->format, depth ? "depth" : "color", gw, gh, format);
      }
    }
    g_surfaces.erase(std::find(g_surfaces.begin(), g_surfaces.end(), s));
    g_surface_at.erase(surface_key(location, offset));
    if (!old) destroy(s);
  }
  s = new Surface{location, offset, format, w, h, depth};
  s->gw = gw;
  s->gh = gh;
  ++g_rs.surfaces;
  auto* dev = device().dev;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  if (depth) {
    bool z16 = format == 1;
    td.Format = z16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    s->dxgi = z16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
    dev->CreateTexture2D(&td, nullptr, &s->tex);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = s->dxgi;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(s->tex, &dd, &s->dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = z16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(s->tex, &sd, &s->srv);
    if (!z16) {
      sd.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
      dev->CreateShaderResourceView(s->tex, &sd, &s->stencil_srv);
    }
  } else {
    s->dxgi = color_dxgi(format);
    td.Format = s->dxgi;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &s->tex);
    dev->CreateRenderTargetView(s->tex, nullptr, &s->rtv);
    dev->CreateShaderResourceView(s->tex, nullptr, &s->srv);
    float zero[4] = {0, 0, 0, 0};
    gpu::rec().ClearRenderTargetView(s->rtv, zero);
  }
  if (depth && s->dsv) gpu::rec().ClearDepthStencilView(s->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  s->last_frame = g_frame;
  g_surfaces.push_back(s);
  g_surface_at[surface_key(location, offset)] = s;
  if (old) {
    stretch_copy(old, s);
    s->written = old->written;
    destroy(old);
  }
  LOG_D("gpu", "surface %s %ux%u fmt %u at %s 0x%08x", depth ? "depth" : "color", w, h, format, location ? "main" : "local",
        offset);
  return s;
}

// width/height: the clip size in pixels. Antialiased surfaces (SURFACE_FORMAT [15:12]) are stored
// with sx * sy samples per pixel as a super-sized image (rsx_facts.md 6.6); they are rendered at
// that storage size (supersampling), so the game's own resolve reads real samples.
std::string g_rt_label = "-";  // the current render target set (--gpu-timing)

struct Targets {
  Surface* color[4] = {};
  u32 count = 0;
  Surface* depth = nullptr;
  u32 width = 0, height = 0, color_format = 0, depth_format = 0;
  u32 sx = 1, sy = 1;    // antialiasing: samples per pixel in x and y (the game's storage is clip size * s)
  u32 sw = 0, sh = 0;    // the bound targets' storage size
  float fx = 1, fy = 1;  // storage pixels per clip pixel (antialiasing times the render scale)
  bool valid = false;
};

Targets bind_targets(bool want_depth) {
  Targets t;
  u32 sf = reg(m::SET_SURFACE_FORMAT);
  t.color_format = sf & 0x1f;
  t.depth_format = (sf >> 5) & 7;
  t.width = reg(m::SET_SURFACE_CLIP_HORIZONTAL) >> 16;
  t.height = reg(m::SET_SURFACE_CLIP_VERTICAL) >> 16;
  if (!t.width || !t.height || t.width > 4096 || t.height > 4096) return t;
  u32 aa = (sf >> 12) & 15;  // 0 CENTER_1, 3 DIAGONAL_CENTERED_2, 4 SQUARE_CENTERED_4, 5 SQUARE_ROTATED_4
  t.sx = aa == 3 || aa == 4 || aa == 5 ? 2 : 1;
  t.sy = aa == 4 || aa == 5 ? 2 : 1;
  if (aa == 5) warn_once("aa-rotated", "SQUARE_ROTATED_4 surfaces are rendered with a square sample grid");
  const u32 gsw = t.width * t.sx, gsh = t.height * t.sy;  // the game's storage size
  static const u32 kOffsetReg[4] = {m::SET_SURFACE_COLOR_AOFFSET, m::SET_SURFACE_COLOR_BOFFSET, m::SET_SURFACE_COLOR_COFFSET,
                                    m::SET_SURFACE_COLOR_DOFFSET};
  static const u32 kDmaReg[4] = {m::SET_CONTEXT_DMA_COLOR_A, m::SET_CONTEXT_DMA_COLOR_B, m::SET_CONTEXT_DMA_COLOR_C,
                                 m::SET_CONTEXT_DMA_COLOR_D};
  u32 target = reg(m::SET_SURFACE_COLOR_TARGET);
  u32 idx[4], n = 0;
  switch (target) {
    case 1: idx[n++] = 0; break;
    case 2: idx[n++] = 1; break;
    case 0x13: idx[n++] = 0; idx[n++] = 1; break;
    case 0x17: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; break;
    case 0x1F: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; idx[n++] = 3; break;
    default: break;
  }
  if ((sf >> 8 & 15) == 2) warn_once("swizzled-rt", "swizzled render targets are rendered as linear surfaces");
  for (u32 i = 0; i < n; ++i)
    t.color[t.count++] = get_surface(dma_location(reg(kDmaReg[idx[i]])), reg(kOffsetReg[idx[i]]), t.color_format, gsw, gsh, false);
  if (want_depth && t.depth_format)
    t.depth = get_surface(dma_location(reg(m::SET_CONTEXT_DMA_ZETA)), reg(m::SET_SURFACE_ZETA_OFFSET), t.depth_format, gsw, gsh, true);
  const Surface* first = t.count ? t.color[0] : t.depth;
  const u32 sw = first ? first->width : gsw, shh = first ? first->height : gsh;
  t.sw = sw;
  t.sh = shh;
  t.fx = float(sw) / float(t.width);
  t.fy = float(shh) / float(t.height);
  g_last_area = (t.fx / float(t.sx)) * (t.fy / float(t.sy));
  ID3D11RenderTargetView* rtvs[4] = {};
  for (u32 i = 0; i < t.count; ++i) rtvs[i] = t.color[i]->rtv;
  ID3D11DepthStencilView* dsv = t.depth ? t.depth->dsv : nullptr;
  t.valid = t.count || t.depth;
  if (g_sh.rt_count == t.count && !std::memcmp(g_sh.rtv, rtvs, sizeof rtvs) && g_sh.dsv == dsv && g_sh.vp_w == sw && g_sh.vp_h == shh)
    return t;  // bound already
  ++g_rs.target_binds;
  if (gpu::timing::enabled()) {  // GPU time is charged per render target set (--gpu-timing)
    g_rt_label = ac4::strprintf("%ux%u c%ux%u%s", t.width, t.height, t.color_format, t.count,
                                 t.depth ? ac4::strprintf(" z%u", t.depth_format).c_str() : "");
    gpu::timing::mark(g_rt_label);
  }
  // Unbind any SRV that aliases a target before binding it for output.
  ID3D11ShaderResourceView* none[16] = {};
  gpu::rec().PSSetShaderResources(0, 16, none);
  std::memset(g_sh.ps_srv, 0, sizeof g_sh.ps_srv);
  gpu::rec().OMSetRenderTargets(t.count, rtvs, dsv);
  D3D11_VIEWPORT vp{0, 0, float(sw), float(shh), 0, 1};
  gpu::rec().RSSetViewports(1, &vp);
  std::memcpy(g_sh.rtv, rtvs, sizeof rtvs);
  g_sh.dsv = dsv;
  g_sh.rt_count = t.count;
  g_sh.vp_w = sw;
  g_sh.vp_h = shh;
  return t;
}

// Scissor rectangle: SCISSOR ∩ VIEWPORT (rsx_facts.md §7.7), clamped to the surface.
D3D11_RECT scissor_rect(const Targets& t, bool clear) {
  u32 sh = reg(m::SET_SCISSOR_HORIZONTAL), sv = reg(m::SET_SCISSOR_VERTICAL);
  LONG x0 = sh & 0xffff, y0 = sv & 0xffff, x1 = x0 + (sh >> 16), y1 = y0 + (sv >> 16);
  if (!clear) {
    u32 vh = reg(m::SET_VIEWPORT_HORIZONTAL), vv = reg(m::SET_VIEWPORT_VERTICAL);
    x0 = std::max<LONG>(x0, vh & 0xffff);
    y0 = std::max<LONG>(y0, vv & 0xffff);
    x1 = std::min<LONG>(x1, (vh & 0xffff) + (vh >> 16));
    y1 = std::min<LONG>(y1, (vv & 0xffff) + (vv >> 16));
  }
  D3D11_RECT r{std::max<LONG>(x0, 0), std::max<LONG>(y0, 0), std::min<LONG>(x1, LONG(t.width)), std::min<LONG>(y1, LONG(t.height))};
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  // Storage pixels (the same rounding as the surface size, so a full rectangle is the whole surface).
  r.left = std::lround(r.left * t.fx), r.right = std::lround(r.right * t.fx);
  r.top = std::lround(r.top * t.fy), r.bottom = std::lround(r.bottom * t.fy);
  return r;
}

// ---------------------------------------------------------------------------------------------
// Clears (rsx_facts.md §7.8). Partial-channel or scissored color clears draw a quad.
ID3D11VertexShader* g_clear_vs = nullptr;
ID3D11PixelShader* g_clear_ps = nullptr;
ID3D11Buffer* g_clear_cb = nullptr;
const char* kClearHlsl = R"(
cbuffer C : register(b0) { float4 color; float4 depth; };
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), depth.x, 1);
}
float4 ps_main() : SV_Target0 { return color; }
)";

ID3D11BlendState* blend_state(const D3D11_BLEND_DESC& d);
ID3D11DepthStencilState* depth_state(const D3D11_DEPTH_STENCIL_DESC& d);
ID3D11RasterizerState* raster_state(const D3D11_RASTERIZER_DESC& d);

void fr_clear(const Targets& t, u32 mask);
void clear_surface(u32 mask) {
  PhaseScope phase_scope(kPhSurfaces);
  Targets t = bind_targets(mask & 3);
  if (!t.valid) return;
  fr_clear(t, mask);
  auto* c = &gpu::rec();
  u32 cv = reg(m::SET_COLOR_CLEAR_VALUE);
  float col[4] = {((cv >> 16) & 0xff) / 255.0f, ((cv >> 8) & 0xff) / 255.0f, (cv & 0xff) / 255.0f, (cv >> 24) / 255.0f};
  if (t.color_format == 16 || t.color_format == 14 || t.color_format == 15) std::swap(col[0], col[2]);  // ABGR formats
  D3D11_RECT r = scissor_rect(t, true);
  bool full = r.left == 0 && r.top == 0 && r.right == LONG(t.sw) && r.bottom == LONG(t.sh);
  u32 cmask = (mask >> 4) & 0xf;  // R G B A in bits 0..3
  if (cmask && t.count) {
    if (full && cmask == 0xf) {
      for (u32 i = 0; i < t.count; ++i) c->ClearRenderTargetView(t.color[i]->rtv, col);
    } else {
      D3D11_BLEND_DESC bd{};
      for (int i = 0; i < 8; ++i) bd.RenderTarget[i].RenderTargetWriteMask = UINT8(cmask);  // R=1 G=2 B=4 A=8
      float data[8] = {col[0], col[1], col[2], col[3], 0, 0, 0, 0};
      c->UpdateSubresource(g_clear_cb, 0, nullptr, data, 0, 0, sizeof data);
      D3D11_DEPTH_STENCIL_DESC dd{};
      D3D11_RASTERIZER_DESC rd{};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.ScissorEnable = TRUE;
      rd.DepthClipEnable = TRUE;
      c->OMSetBlendState(blend_state(bd), nullptr, 0xffffffff);
      c->OMSetDepthStencilState(depth_state(dd), 0);
      c->RSSetState(raster_state(rd));
      c->RSSetScissorRects(1, &r);
      c->IASetInputLayout(nullptr);
      c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      c->VSSetShader(g_clear_vs, nullptr, 0);
      c->PSSetShader(g_clear_ps, nullptr, 0);
      c->VSSetConstantBuffers(0, 1, &g_clear_cb);
      c->PSSetConstantBuffers(0, 1, &g_clear_cb);
      c->Draw(3, 0);
      shadow_reset();
    }
  }
  if ((mask & 3) && t.depth) {
    u32 zs = reg(m::SET_ZSTENCIL_CLEAR_VALUE);
    float z = t.depth_format == 1 ? (zs & 0xffff) / 65535.0f : (zs >> 8) / 16777215.0f;
    bool zc = mask & 1, sc = (mask & 2) && t.depth_format == 2;
    UINT flags = (zc ? D3D11_CLEAR_DEPTH : 0) | (sc ? D3D11_CLEAR_STENCIL : 0);
    if (full) {
      if (flags) c->ClearDepthStencilView(t.depth->dsv, flags, z, UINT8(zs & 0xff));
    } else if (flags) {
      // Scissored: a quad that writes the clear values inside the scissor rectangle only.
      D3D11_BLEND_DESC bd{};  // no color writes
      float data[8] = {0, 0, 0, 0, z, 0, 0, 0};
      c->UpdateSubresource(g_clear_cb, 0, nullptr, data, 0, 0, sizeof data);
      D3D11_DEPTH_STENCIL_DESC dd{};
      dd.DepthEnable = zc;
      dd.DepthWriteMask = zc ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
      dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
      dd.StencilEnable = sc;
      dd.StencilReadMask = 0xff;
      dd.StencilWriteMask = 0xff;
      dd.FrontFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
      dd.BackFace = dd.FrontFace;
      D3D11_RASTERIZER_DESC rd{};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.ScissorEnable = TRUE;
      rd.DepthClipEnable = FALSE;
      c->OMSetBlendState(blend_state(bd), nullptr, 0xffffffff);
      c->OMSetDepthStencilState(depth_state(dd), zs & 0xff);
      c->RSSetState(raster_state(rd));
      c->RSSetScissorRects(1, &r);
      c->IASetInputLayout(nullptr);
      c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      c->VSSetShader(g_clear_vs, nullptr, 0);
      c->PSSetShader(g_clear_ps, nullptr, 0);
      c->VSSetConstantBuffers(0, 1, &g_clear_cb);
      c->PSSetConstantBuffers(0, 1, &g_clear_cb);
      c->Draw(3, 0);
      shadow_reset();
    }
  }
  ++g_write_serial;
  for (u32 i = 0; i < t.count; ++i) t.color[i]->written = g_write_serial;
  if (t.depth) t.depth->written = g_write_serial;
}

// ---------------------------------------------------------------------------------------------
// State object caches
template <typename Desc, typename Obj>
struct StateCache {
  std::unordered_map<u64, Obj*> map;
  template <typename F>
  Obj* get(const Desc& d, F create) {
    u64 h = hash_bytes(reinterpret_cast<const u8*>(&d), sizeof d);
    auto it = map.find(h);
    if (it != map.end()) return it->second;
    Obj* o = nullptr;
    create(&d, &o);
    map[h] = o;
    return o;
  }
};
StateCache<D3D11_BLEND_DESC, ID3D11BlendState> g_blend;
StateCache<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState> g_depth;
StateCache<D3D11_RASTERIZER_DESC, ID3D11RasterizerState> g_raster;
StateCache<D3D11_SAMPLER_DESC, ID3D11SamplerState> g_sampler;

ID3D11BlendState* blend_state(const D3D11_BLEND_DESC& d) {
  return g_blend.get(d, [](const D3D11_BLEND_DESC* p, ID3D11BlendState** o) { device().dev->CreateBlendState(p, o); });
}
ID3D11DepthStencilState* depth_state(const D3D11_DEPTH_STENCIL_DESC& d) {
  return g_depth.get(d, [](const D3D11_DEPTH_STENCIL_DESC* p, ID3D11DepthStencilState** o) { device().dev->CreateDepthStencilState(p, o); });
}
ID3D11RasterizerState* raster_state(const D3D11_RASTERIZER_DESC& d) {
  return g_raster.get(d, [](const D3D11_RASTERIZER_DESC* p, ID3D11RasterizerState** o) { device().dev->CreateRasterizerState(p, o); });
}
ID3D11SamplerState* sampler_state(const D3D11_SAMPLER_DESC& d) {
  return g_sampler.get(d, [](const D3D11_SAMPLER_DESC* p, ID3D11SamplerState** o) { device().dev->CreateSamplerState(p, o); });
}

D3D11_BLEND blend_factor(u32 f, bool alpha) {
  switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
  }
}
D3D11_BLEND_OP blend_op(u32 e) {
  switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF005: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
  }
}
D3D11_COMPARISON_FUNC cmp_func(u32 f) { return D3D11_COMPARISON_FUNC((f & 7) + 1); }
D3D11_STENCIL_OP stencil_op(u32 o) {
  switch (o) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
  }
}
D3D11_TEXTURE_ADDRESS_MODE wrap_mode(u32 w) {
  switch (w) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 6: case 7: case 8: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
  }
}

// ---------------------------------------------------------------------------------------------
// Programs
u32 g_vp[gpu::kVpSlots * 4];
u32 g_vp_load = 0, g_vp_burst = 0;
u32 g_vp_gen = 0;  // bumped when a program word changes
float g_vc[512 * 4];
u32 g_vc_load = 0;
// Registers changed since the last upload: only [g_vc_lo, g_vc_hi] is recorded (Recorder::
// UpdateConstants). Games set a few registers per draw (a matrix); recording the whole 7.5 KiB
// buffer per draw was ~7% of the FIFO thread.
bool g_vc_dirty = true, g_vc_whole = true;
u32 g_vc_lo = ~0u, g_vc_hi = 0;
u32 g_patch_cb = 7;       // "gpu patch MASK": 1 vertex constants, 2 fragment constants, 4 vertex formats (A/B)
bool g_cb_reset = false;  // next draw records the constant buffers whole

struct VsEntry {
  ID3D11VertexShader* vs = nullptr;
  gpu::VpInfo info;
};
std::unordered_map<u64, VsEntry> g_vs;
struct PsEntry {
  ID3D11PixelShader* ps = nullptr;
  gpu::FpInfo info;
};
std::unordered_map<u64, PsEntry> g_ps;

ID3D11Buffer* g_vc_buf = nullptr;    // b0
ID3D11Buffer* g_vs_state = nullptr;  // b1
ID3D11Buffer* g_fp_state = nullptr;  // b2
ID3D11Buffer* g_fp_const = nullptr;  // b3

// Diagnostics (--dump-shaders): generated HLSL in <run>/shaders/<kind>_<key>.hlsl.
void dump_shader(const char* kind, u64 key, const std::string& src) {
  if (!cfg().dump_shaders) return;
  std::filesystem::path dir = std::filesystem::path(cfg().run_dir) / "shaders";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  ac4::write_file((dir / ac4::strprintf("%s_%016llx.hlsl", kind, (unsigned long long)key)).string(),
                   std::vector<u8>(src.begin(), src.end()));
}

// Shader records (--shader-collect): each program translated in this run with what its HLSL
// depends on, for the shader manifest (tools/shader_manifest.py finds the programs in the game's
// shader archives and keeps positions only; the pipeline compiles them before a new installation's
// first run). Game-derived: local\shader_records, never in Git. Layout (little-endian):
// "SHREC1\0\0", kind ('v'/'f'), 3 reserved bytes, env bytes, start slot, code bytes, constant slot
// count (u32 each), then env, the constant slots' byte offsets (u32 each; their values are patched
// at run time and not part of the HLSL) and the code (VP: big-endian words as in the command
// buffer; FP: guest memory bytes). Named by a hash of everything with the constant slots cleared.
void record_shader(char kind, const void* env, u32 env_size, u32 start, const u8* code, u32 code_size,
                   const std::vector<u32>& const_slots) {
  if (!cfg().shader_collect) return;
  std::vector<u8> masked(code, code + code_size);
  for (u32 off : const_slots)
    if (off + 16 <= code_size) std::memset(masked.data() + off, 0, 16);
  std::vector<u8> key;
  key.push_back(u8(kind));
  key.insert(key.end(), static_cast<const u8*>(env), static_cast<const u8*>(env) + env_size);
  key.insert(key.end(), reinterpret_cast<const u8*>(&start), reinterpret_cast<const u8*>(&start) + 4);
  key.insert(key.end(), masked.begin(), masked.end());
  auto h = ac4::crypto::sha256(key.data(), key.size());
  std::string name;
  for (int i = 0; i < 8; ++i) name += ac4::strprintf("%02x", h[i]);
  std::filesystem::path file = std::filesystem::path(cfg().shader_records) / (name + ".shrec");
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) return;
  std::vector<u8> out = {'S', 'H', 'R', 'E', 'C', '1', 0, 0, u8(kind), 0, 0, 0};
  auto put = [&](u32 v) { out.insert(out.end(), reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + 4); };
  put(env_size);
  put(start);
  put(code_size);
  put(u32(const_slots.size()));
  out.insert(out.end(), static_cast<const u8*>(env), static_cast<const u8*>(env) + env_size);
  for (u32 off : const_slots) put(off);
  out.insert(out.end(), code, code + code_size);
  try {
    std::filesystem::create_directories(file.parent_path(), ec);
    ac4::write_file(file.string(), out);
  } catch (const std::exception& e) {
    warn_once("shrec", std::string("shader record not written: ") + e.what());
  }
}

const VsEntry* vertex_shader() {
  u32 start = reg(m::SET_TRANSFORM_PROGRAM_START) % gpu::kVpSlots;
  static u32 memo_start = ~0u, memo_gen = ~0u;
  static const VsEntry* memo = nullptr;
  if (memo && start == memo_start && g_vp_gen == memo_gen) return memo;
  memo_start = start;
  memo_gen = g_vp_gen;
  // Key: the program words from the start until END (and any branch targets: whole memory if branchy).
  u64 h = hash_bytes(reinterpret_cast<const u8*>(&start), 4);
  u32 end = start;
  bool branchy = false;
  for (u32 pc = start; pc < gpu::kVpSlots; ++pc) {
    u32 sop = (g_vp[4 * pc + 1] >> 27) & 0x1f;
    if (sop >= 8 && sop <= 12) branchy = true;
    if (sop == 0x11 || sop == 0x12) branchy = true;
    end = pc;
    if (g_vp[4 * pc + 3] & 1) break;
  }
  h ^= branchy ? hash_bytes(reinterpret_cast<const u8*>(g_vp), sizeof g_vp)
               : hash_bytes(reinterpret_cast<const u8*>(&g_vp[4 * start]), 16 * (end - start + 1)) * 0x9e3779b97f4a7c15ull;
  auto it = g_vs.find(h);
  if (it != g_vs.end()) return memo = &it->second;
  VsEntry e;
  std::string src = gpu::vp_to_hlsl(g_vp, start, &e.info);
  dump_shader("vp", h, src);
  if (cfg().shader_collect && !branchy) {  // branchy programs depend on all of program memory: not recorded
    std::vector<u8> words(size_t(end - start + 1) * 16);
    for (u32 i = 0; i < (end - start + 1) * 4; ++i) {
      u32 be = __builtin_bswap32(g_vp[4 * start + i]);
      std::memcpy(&words[4 * i], &be, 4);
    }
    record_shader('v', nullptr, 0, start, words.data(), u32(words.size()), {});
  }
  if (e.info.error.empty()) {
    if (ID3DBlob* b = gpu::compile_hlsl(src, "main", "vs_5_0")) {
      device().dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &e.vs);
      b->Release();
    } else {
      e.info.error = "HLSL compilation failed";
      LOG_D("gpu", "VP source:\n%s", src.c_str());
    }
  }
  if (!e.info.error.empty()) LOG_W("gpu", "vertex program at slot %u not usable: %s", start, e.info.error.c_str());
  return memo = &(g_vs[h] = e);
}

const PsEntry* pixel_shader(const gpu::FpEnv& env, const u8* ucode, u32 max_bytes, u64 code_hash, u32 sp) {
  u64 h = fnv(&env, sizeof env, code_hash);
  auto it = g_ps.find(h);
  if (it != g_ps.end()) return &it->second;
  PsEntry e;
  std::string src = gpu::fp_to_hlsl(ucode, max_bytes, env, &e.info);
  dump_shader("fp", h, ac4::strprintf("// SET_SHADER_PROGRAM 0x%08x\n", sp) + src);
  if (cfg().shader_collect) {
    std::vector<u32> offs;
    u32 size = gpu::fp_scan(ucode, max_bytes, nullptr, &offs);
    u32 base = 0;  // constant offsets are relative to the first non-NOP instruction (flush_draw)
    for (; base + 16 <= max_bytes && ((gpu::fp_word(ucode + base) >> 24) & 0x3f) == 0; base += 16) {
    }
    for (u32& o : offs) o += base;
    if (size) record_shader('f', &env, sizeof env, 0, ucode, size, offs);
  }
  if (e.info.error.empty()) {
    if (ID3DBlob* b = gpu::compile_hlsl(src, "main", "ps_5_0")) {
      device().dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &e.ps);
      b->Release();
    } else {
      e.info.error = "HLSL compilation failed";
      LOG_D("gpu", "FP source:\n%s", src.c_str());
    }
  }
  if (!e.info.error.empty()) LOG_W("gpu", "fragment program not usable: %s", e.info.error.c_str());
  return &(g_ps[h] = e);
}

// ---------------------------------------------------------------------------------------------
// Textures
struct TexEntry {
  ID3D11Resource* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  u64 hash = 0, checked_frame = ~0ull;
  u32 checked_serial = 0;  // write-tracking serial when the contents were last verified
  u32 w = 0, h = 0, d = 1, mips = 0, faces = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  u64 bytes = 0, last_used = 0;  // host texture size; frame of the last lookup (cache trimming)
  // Contents changed after the first upload (movie frames, render-to-memory copies): DEFAULT usage,
  // updated in place in command order, no hashing when its pages were written.
  bool dynamic = false;
};

// Guest page write tracking: per 4 KiB page, the serial of the last collection that saw it
// written (or of an RSX transfer that wrote it, renderer_memory_written).
std::vector<u32> g_page_serial;
u32 g_mem_serial = 1;
bool g_track_writes = true;
// Only the 64 KiB granules holding cached data are queried (a query costs in proportion to the
// range). A collection queries, in runs, the granules a cache check used since the previous
// collection (hot mode 1; mode 0: in the last kHotFrames frames: m540 0.66 instead of 0.37 ms per
// frame); the others keep their written state in the OS (it accumulates until reset) and are
// queried when a check next touches them (refresh_granules), so data idle for a while costs
// nothing.
constexpr u32 kGranuleShift = 16;
constexpr u32 kGranules = 1u << (32 - kGranuleShift);
constexpr u64 kHotFrames = 2;
u32 g_hot_mode = 1;  // "gpu hot 0|1" (A/B)
// Runs of hot granules this many granules apart or closer are queried in one call: a query costs
// ~3 us per call and little per byte (m540: 504 calls over 268 MiB per frame took 1.7 ms), and
// pages found written in the gaps are recorded like any others. "gpu wwgap N" (A/B).
u32 g_ww_gap = 2;
std::vector<u8> g_granule_watched(kGranules);
std::vector<u32> g_watched_list;  // the watched granules (sorted at the next collection)
bool g_watched_sorted = true;
std::vector<u32> g_granule_queried(kGranules);     // collection epoch of the last query
std::vector<u64> g_granule_used(kGranules);        // frame of the last cache check touching it
std::vector<u32> g_granule_used_epoch(kGranules);  // collection epoch of the last cache check touching it
u32 g_collect_epoch = 0;
void watch_range(u32 ea, u32 size) {
  for (u32 r = ea >> kGranuleShift, e = u32((u64(ea) + size + (1u << kGranuleShift) - 1) >> kGranuleShift); r < e && r < kGranules;
       ++r)
    if (!g_granule_watched[r]) {
      g_granule_watched[r] = 1;
      g_watched_list.push_back(r);
      g_watched_sorted = false;
    }
}
bool query_pages(u32 first, u32 end, std::vector<u32>& pages) {
  g_rs.bytes_queried += u64(end - first) << kGranuleShift;
  ++g_rs.query_calls;
  size_t before = pages.size();
  u64 t0 = __rdtsc();
  bool ok = vm::take_written_pages(first << kGranuleShift, (end - first) << kGranuleShift, pages);
  g_rs.query_ticks += __rdtsc() - t0;
  g_rs.query_written += pages.size() - before;
  if (ok) return true;
  g_track_writes = false;
  LOG_W("gpu", "guest write tracking query failed: cached textures and vertices are re-checked by hashing");
  return false;
}
// Brings the page serials of [ea, ea + size) up to date for this collection epoch. Pages found
// written get a new serial, so anything verified before (even in this epoch) counts as stale.
void refresh_granules(u32 ea, u32 size) {
  static std::vector<u32> pages;
  u32 r = ea >> kGranuleShift, e = u32(std::min<u64>((u64(ea) + size + (1u << kGranuleShift) - 1) >> kGranuleShift, kGranules));
  for (; r < e; ++r) {
    g_granule_used[r] = g_frame;
    g_granule_used_epoch[r] = g_collect_epoch;
    if (!g_granule_watched[r] || g_granule_queried[r] == g_collect_epoch) continue;
    u32 run_end = r + 1;
    while (run_end < e && g_granule_watched[run_end] && g_granule_queried[run_end] != g_collect_epoch) {
      g_granule_used[run_end] = g_frame;
      g_granule_used_epoch[run_end++] = g_collect_epoch;
    }
    pages.clear();
    if (!query_pages(r, run_end, pages)) return;
    for (u32 k = r; k < run_end; ++k) g_granule_queried[k] = g_collect_epoch;
    if (!pages.empty()) {
      ++g_mem_serial;
      for (u32 a : pages) g_page_serial[a >> 12] = g_mem_serial;
    }
    r = run_end - 1;
  }
}
// Collections are lazy: one runs before the next cache lookup once commands may read data written
// after the previous one. That is the case when the FIFO reaches the put pointer read for the
// previous collection (later commands were published, so their data written, after it), after a
// semaphore acquire (the guest may write data before releasing it) and at frame boundaries.
bool g_writes_stale = true;
void collect_writes() {
  if (!g_writes_stale) return;
  PhaseScope phase_scope(kPhWriteChecks);
  g_writes_stale = false;
  g_covered_put = ppu::r32(kControlAddr);  // read before the query: commands up to it are covered
  if (!g_track_writes) return;
  ++g_rs.collections;
  u32 ended = g_collect_epoch++;  // every granule is unqueried in the new epoch
  if (g_page_serial.empty()) g_page_serial.assign(1u << 20, 0);
  if (!g_watched_sorted) {
    std::sort(g_watched_list.begin(), g_watched_list.end());
    g_watched_sorted = true;
  }
  // Hot granules in runs; small gaps are queried too (fewer calls for about the same cost).
  static std::vector<std::pair<u32, u32>> runs;
  runs.clear();
  for (u32 r : g_watched_list) {
    bool hot = g_hot_mode ? g_granule_used_epoch[r] == ended : g_granule_used[r] + kHotFrames >= g_frame;
    if (!hot) continue;
    if (!runs.empty() && r - runs.back().second <= g_ww_gap) runs.back().second = r + 1;
    else runs.push_back({r, r + 1});
  }
  static std::vector<u32> pages;
  pages.clear();
  for (auto [b, e] : runs) {
    if (!query_pages(b, e, pages)) return;
    for (u32 k = b; k < e; ++k) g_granule_queried[k] = g_collect_epoch;
  }
  ++g_mem_serial;
  for (u32 a : pages) g_page_serial[a >> 12] = g_mem_serial;
}
// True if a page of [ea, ea + size) was written after `serial`.
bool written_since(u32 ea, u32 size, u32 serial) {
  if (!g_track_writes || g_page_serial.empty()) return true;
  refresh_granules(ea, size);
  if (!g_track_writes) return true;
  for (u32 p = ea >> 12, e = u32((u64(ea) + size + 0xfff) >> 12); p < e; ++p)
    if (g_page_serial[p] > serial) return true;
  return false;
}
std::unordered_map<u64, TexEntry> g_tex;  // key: offset/location/format/dims
u64 g_tex_bytes = 0;
constexpr u64 kTexBudget = 512ull << 20;

// Each mission brings its own textures: once the budget is exceeded, entries unused for ten
// seconds are dropped (least recently used first) until it holds a quarter less.
void tex_trim() {
  static u64 next_scan = 0;  // while every texture is in use, rescan once a second
  if (g_tex_bytes <= kTexBudget || g_frame < next_scan) return;
  next_scan = g_frame + 60;
  std::vector<std::pair<u64, u64>> idle;  // last use, key
  for (const auto& [key, e] : g_tex)
    if (e.last_used + 600 < g_frame) idle.push_back({e.last_used, key});
  std::sort(idle.begin(), idle.end());
  u64 dropped = 0, before = g_tex_bytes;
  for (const auto& [used, key] : idle) {
    if (g_tex_bytes <= kTexBudget / 4 * 3) break;
    TexEntry& e = g_tex[key];
    release(e.srv);
    release(e.tex);
    g_tex_bytes -= e.bytes;
    g_tex.erase(key);
    ++dropped;
  }
  if (dropped) LOG_I("gpu", "texture cache: %llu idle texture(s) dropped, %.0f -> %.0f MiB", (unsigned long long)dropped, before / 1048576.0,
                     g_tex_bytes / 1048576.0);
}

// A draw that samples a surface it is rendering to reads the surface as it was before the draw
// (exact for the per-pixel read-modify-write such passes do): it samples a copy.
ID3D11ShaderResourceView* feedback_copy(Surface* s) {
  if (!s->copy) {
    D3D11_TEXTURE2D_DESC td{};
    s->tex->GetDesc(&td);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    device().dev->CreateTexture2D(&td, nullptr, &s->copy);
    if (!s->copy) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    s->srv->GetDesc(&sd);
    device().dev->CreateShaderResourceView(s->copy, &sd, &s->copy_srv);
  }
  if (s->copy_serial != s->written) {
    gpu::timing::mark(ac4::strprintf("feedback copy %ux%u", s->width, s->height));
    gpu::rec().CopyResource(s->copy, s->tex);
    gpu::timing::mark(g_rt_label);
    s->copy_serial = s->written;
  }
  return s->copy_srv;
}

// D24S8 depth sampled as A8R8G8B8: the console stores (Z << 8) | S per pixel, so the texel bytes
// are A = Z[23:16], R = Z[15:8], G = Z[7:0], B = S. A full-screen pass rebuilds those bytes.
ID3D11VertexShader* g_d2c_vs = nullptr;
ID3D11PixelShader* g_d2c_ps = nullptr;
const char* kDepthToColorHlsl = R"(
Texture2D<float> depth_tex : register(t0);
Texture2D<uint2> stencil_tex : register(t1);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target0 {
  int3 p = int3(pos.xy, 0);
  uint z = (uint)round(saturate(depth_tex.Load(p)) * 16777215.0);
  uint s = stencil_tex.Load(p).y;
  return float4(((z >> 8) & 255) / 255.0, (z & 255) / 255.0, s / 255.0, (z >> 16) / 255.0);
}
)";
// A full-screen pass into `out` (w x h pixels) with the given shaders and inputs. It is recorded
// as one call that saves and restores every state it changes: passes run while a draw is being
// prepared, in stream order (submit.h), where the state they save is current.
void fullscreen_pass(ID3D11RenderTargetView* out, u32 w, u32 h, ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                     ID3D11ShaderResourceView* in0, ID3D11ShaderResourceView* in1, ID3D11SamplerState* smp) {
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_DEPTH_STENCIL_DESC dd{};
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  ID3D11BlendState* pass_bs = blend_state(bd);
  ID3D11DepthStencilState* pass_ds = depth_state(dd);
  ID3D11RasterizerState* pass_rs = raster_state(rd);
  gpu::rec().call([=](ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* rtvs[4] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(4, rtvs, &dsv);
    D3D11_VIEWPORT vp[1];
    UINT nvp = 1;
    c->RSGetViewports(&nvp, vp);
    ID3D11BlendState* old_bs = nullptr;
    float old_bf[4];
    UINT old_mask = 0;
    c->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ID3D11DepthStencilState* old_ds = nullptr;
    UINT old_ref = 0;
    c->OMGetDepthStencilState(&old_ds, &old_ref);
    ID3D11RasterizerState* old_rs = nullptr;
    c->RSGetState(&old_rs);
    D3D11_RECT old_sc[1];
    UINT nsc = 1;
    c->RSGetScissorRects(&nsc, old_sc);
    D3D11_PRIMITIVE_TOPOLOGY old_topo;
    c->IAGetPrimitiveTopology(&old_topo);
    ID3D11VertexShader* old_vs = nullptr;
    ID3D11PixelShader* old_ps = nullptr;
    c->VSGetShader(&old_vs, nullptr, nullptr);
    c->PSGetShader(&old_ps, nullptr, nullptr);
    ID3D11ShaderResourceView* old_srv[2] = {};
    c->PSGetShaderResources(0, 2, old_srv);
    ID3D11SamplerState* old_smp = nullptr;
    c->PSGetSamplers(0, 1, &old_smp);
    ID3D11ShaderResourceView* none[2] = {};
    c->PSSetShaderResources(0, 2, none);  // the output may be bound as an input of the pending draw
    c->OMSetRenderTargets(1, &out, nullptr);
    D3D11_VIEWPORT v{0, 0, float(w), float(h), 0, 1};
    c->RSSetViewports(1, &v);
    c->OMSetBlendState(pass_bs, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(pass_ds, 0);
    c->RSSetState(pass_rs);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* in[2] = {in0, in1};
    c->PSSetShaderResources(0, 2, in);
    if (smp) c->PSSetSamplers(0, 1, &smp);
    c->Draw(3, 0);
    c->PSSetShaderResources(0, 2, none);
    c->OMSetRenderTargets(4, rtvs, dsv);
    c->PSSetShaderResources(0, 2, old_srv);
    if (smp) c->PSSetSamplers(0, 1, &old_smp);
    if (nvp) c->RSSetViewports(nvp, vp);
    c->OMSetBlendState(old_bs, old_bf, old_mask);
    c->OMSetDepthStencilState(old_ds, old_ref);
    c->RSSetState(old_rs);
    if (nsc) c->RSSetScissorRects(nsc, old_sc);
    c->IASetPrimitiveTopology(old_topo);
    c->VSSetShader(old_vs, nullptr, 0);
    c->PSSetShader(old_ps, nullptr, 0);
    for (IUnknown* o : {static_cast<IUnknown*>(rtvs[0]), static_cast<IUnknown*>(rtvs[1]), static_cast<IUnknown*>(rtvs[2]),
                        static_cast<IUnknown*>(rtvs[3]), static_cast<IUnknown*>(dsv), static_cast<IUnknown*>(old_bs),
                        static_cast<IUnknown*>(old_ds), static_cast<IUnknown*>(old_rs), static_cast<IUnknown*>(old_vs),
                        static_cast<IUnknown*>(old_ps), static_cast<IUnknown*>(old_srv[0]), static_cast<IUnknown*>(old_srv[1]),
                        static_cast<IUnknown*>(old_smp)})
      if (o) o->Release();  // references taken by the Get calls above (this thread)
  });
  shadow_reset();
}

ID3D11ShaderResourceView* depth_as_color(Surface* s) {
  if (s->format != 2 || !s->stencil_srv) {
    warn_once("z16-as-color", "16-bit depth sampled with a color format: raw depth used");
    return s->srv;
  }
  auto* dev = device().dev;
  if (!g_d2c_vs) {
    ID3DBlob* vs = gpu::compile_hlsl(kDepthToColorHlsl, "vs_main", "vs_5_0");
    ID3DBlob* ps = gpu::compile_hlsl(kDepthToColorHlsl, "ps_main", "ps_5_0");
    if (vs) dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_d2c_vs);
    if (ps) dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_d2c_ps);
    release(vs);
    release(ps);
    if (!g_d2c_vs || !g_d2c_ps) return s->srv;
  }
  if (!s->as_color) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = s->width;
    td.Height = s->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &s->as_color);
    if (!s->as_color) return s->srv;
    dev->CreateRenderTargetView(s->as_color, nullptr, &s->as_color_rtv);
    dev->CreateShaderResourceView(s->as_color, nullptr, &s->as_color_srv);
  }
  if (s->as_color_serial == s->written) return s->as_color_srv;
  gpu::timing::mark(ac4::strprintf("depth as color %ux%u", s->width, s->height));
  fullscreen_pass(s->as_color_rtv, s->width, s->height, g_d2c_vs, g_d2c_ps, s->srv, s->stencil_srv, nullptr);
  gpu::timing::mark(g_rt_label);
  s->as_color_serial = s->written;
  return s->as_color_srv;
}

// A color surface re-created at another render scale gets its old content stretched into it, so
// what the game rendered once (and keeps sampling) survives the change.
ID3D11VertexShader* g_stretch_vs = nullptr;
ID3D11PixelShader* g_stretch_ps = nullptr;
const char* kStretchHlsl = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) {
  V o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps_main(V i) : SV_Target0 { return src.SampleLevel(smp, i.uv, 0); }
)";
void stretch_copy(Surface* from, Surface* to) {
  if (!from->srv || !to->rtv) return;
  auto* dev = device().dev;
  if (!g_stretch_vs) {
    ID3DBlob* vs = gpu::compile_hlsl(kStretchHlsl, "vs_main", "vs_5_0");
    ID3DBlob* ps = gpu::compile_hlsl(kStretchHlsl, "ps_main", "ps_5_0");
    if (vs) dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_stretch_vs);
    if (ps) dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_stretch_ps);
    release(vs);
    release(ps);
    if (!g_stretch_vs || !g_stretch_ps) return;
  }
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  fullscreen_pass(to->rtv, to->width, to->height, g_stretch_vs, g_stretch_ps, from->srv, nullptr, sampler_state(sd));
}

// The render scale for the next frame: the output's 16:9 picture height over 720 times the
// player's render scale, applied once it has held for 20 frames (a window being resized).
void update_render_scale() {
  int w = 0, h = 0;
  gpu::window_client_size(&w, &h);
  if (w <= 0 || h <= 0) return;  // minimized: keep the scale
  int pct = cfg().render_scale ? cfg().render_scale : settings::display().render_scale;
  float fit_h = std::min(float(h), float(w) * 9.0f / 16.0f);
  float target = std::clamp(fit_h / 720.0f * float(pct) / 100.0f, 0.5f, 4.0f);
  static float pending = 0;
  static int stable = 0;
  if (std::abs(target - g_scale) < 0.001f) {
    pending = 0;
    return;
  }
  if (std::abs(target - pending) > 0.001f) {
    pending = target;
    stable = 0;
    return;
  }
  if (++stable < 20) return;
  g_scale = target;
  pending = 0;
  LOG_I("gpu", "render scale %.3f (output %dx%d, render scale %d%%): the game's 1280x720 renders at %ux%u", g_scale, w, h, pct,
        scaled(1280, g_scale), scaled(720, g_scale));
}

// The rendered surface a texture refers to, if any. Rendered data never reaches guest memory
// here, so a texture at a surface's address reads the surface, unless the texture cannot be that
// surface: then the memory was reused for other data. Compressed textures of exactly the
// surface's size are aliased too: the game renders AC paint into such a surface and then samples
// a DXT copy that its own code derives from the rendered pixels.
Surface* texture_surface(const gpu::TexDesc& t) {
  Surface* s = find_surface(t.location, t.offset);
  if (!s) return nullptr;
  bool compressed = t.format == 0x86 || t.format == 0x87 || t.format == 0x88;
  bool depth_fmt = t.format >= 0x90 && t.format <= 0x93;
  if (s->depth) {
    bool ok = (depth_fmt || t.format == 0x85 || t.format == 0x9e) && t.width <= s->gw && t.height <= s->gh;
    return ok ? s : nullptr;
  }
  if (depth_fmt) return nullptr;
  if (compressed) {
    if (t.width != s->gw || t.height != s->gh) return nullptr;
    warn_once("dxt-alias", "compressed textures at rendered surfaces read the rendered pixels (GPU results are not written back)");
    return s;
  }
  return t.width <= s->gw && t.height <= s->gh ? s : nullptr;
}

ID3D11ShaderResourceView* texture_view(const gpu::TexDesc& t, float* scale) {
  scale[0] = t.width ? 1.0f / float(t.width) : 1;
  scale[1] = t.height ? 1.0f / float(t.height) : 1;
  scale[2] = t.depth ? 1.0f / float(t.depth) : 1;
  scale[3] = t.unnormalized ? 1.0f : 0.0f;
  // Render-to-texture: a surface at the same address.
  if (Surface* s = texture_surface(t)) {
    s->last_frame = g_frame;  // in use (surface trimming)
    if (s->depth && (t.format == 0x85 || t.format == 0x9e)) return depth_as_color(s);
    return s->srv;
  }
  u32 size = gpu::texture_size(t);
  u32 ea = offset_to_ea(t.offset, t.location);
  if (!size || !ea) {
    warn_once("tex-fmt-" + std::to_string(t.format), "texture format 0x" + ac4::strprintf("%02x", t.format) + " not supported");
    return nullptr;
  }
  u64 key = fnv(&t.offset, 4, fnv(&t.location, 4, fnv(&t.format, 4, fnv(&t.width, 4, fnv(&t.height, 4, fnv(&t.depth, 4, fnv(&t.mips, 4,
            fnv(&t.remap, 4, fnv(&t.pitch, 4, u64(t.cubemap) * 31 + u64(t.linear) * 7 + t.dim + u64(t.border_color) * 131)))))))));
  TexEntry& e = g_tex[key];
  e.last_used = g_frame;
  collect_writes();
  // Verified since the last collection (without write tracking: this frame): unchanged.
  if (e.srv && (g_track_writes ? e.checked_serial == g_mem_serial : e.checked_frame == g_frame)) return e.srv;
  watch_range(ea, size);
  e.checked_frame = g_frame;
  if (e.srv && !written_since(ea, size, e.checked_serial)) {  // untouched since verified
    e.checked_serial = g_mem_serial;
    return e.srv;
  }
  e.checked_serial = g_mem_serial;
  if (!e.dynamic) {  // a dynamic texture that was written has new contents: no hash needed
    u64 h = hash_bytes(::ppu::mem() + ea, size);
    ++g_rs.tex_hashed;
    if (e.srv && e.hash == h) return e.srv;
    e.hash = h;
  }
  gpu::TexData data;
  ++g_rs.tex_uploaded;
  if (!gpu::convert_texture(t, ::ppu::mem() + ea, &data)) {
    warn_once("tex-cvt-" + std::to_string(t.format), "texture conversion failed: " + data.error);
    return nullptr;
  }
  if ((t.format == 0x86 || t.format == 0x87 || t.format == 0x88) && data.format == DXGI_FORMAT_R8G8B8A8_UNORM && gpu::g_texture_bc)
    warn_once(ac4::strprintf("dxt-cpu-%x-%x-%u", t.format, t.remap, t.dim),
              ac4::strprintf("DXT texture decoded on the CPU (format 0x%02x, remap 0x%04x, %ux%u, dimension %u)", t.format, t.remap,
                             t.width, t.height, t.dim));
  // Same shape as the host texture of a dynamic entry: update it in place, in command order. The
  // converted levels move into the recorded call (no copy into the recorder: movie frames).
  if (e.dynamic && e.tex && e.w == data.width && e.h == data.height && e.d == data.depth && e.mips == data.mips &&
      e.faces == data.faces && e.fmt == data.format) {
    gpu::rec().call([tex = e.tex, levels = std::move(data.levels)](ID3D11DeviceContext* c) {
      for (size_t i = 0; i < levels.size(); ++i)
        c->UpdateSubresource(tex, UINT(i), nullptr, levels[i].data.data(), levels[i].row_pitch, levels[i].slice_pitch);
    });
    return e.srv;
  }
  bool changed = e.srv != nullptr;  // new contents for an entry that had others: it changes (will again)
  release(e.srv);
  release(e.tex);
  e.srv = nullptr;
  e.tex = nullptr;
  g_tex_bytes -= e.bytes;
  e.bytes = 0;
  e.dynamic = e.dynamic || changed;
  std::vector<D3D11_SUBRESOURCE_DATA> init;
  for (auto& L : data.levels) init.push_back({L.data.data(), L.row_pitch, L.slice_pitch});
  auto* dev = device().dev;
  D3D11_USAGE usage = e.dynamic ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
  if (t.dim == 3) {
    D3D11_TEXTURE3D_DESC td{data.width, data.height, data.depth, data.mips, data.format, usage, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture3D* t3 = nullptr;
    dev->CreateTexture3D(&td, init.data(), &t3);
    e.tex = t3;
  } else {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = data.width;
    td.Height = data.height;
    td.MipLevels = data.mips;
    td.ArraySize = data.faces;
    td.Format = data.format;
    td.SampleDesc.Count = 1;
    td.Usage = usage;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = t.cubemap ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    ID3D11Texture2D* t2 = nullptr;
    dev->CreateTexture2D(&td, init.data(), &t2);
    e.tex = t2;
  }
  if (e.tex) {
    dev->CreateShaderResourceView(e.tex, nullptr, &e.srv);
    for (auto& L : data.levels) e.bytes += L.data.size();
    g_tex_bytes += e.bytes;
    e.w = data.width;
    e.h = data.height;
    e.d = data.depth;
    e.mips = data.mips;
    e.faces = data.faces;
    e.fmt = data.format;
  } else {
    warn_once("tex-create", ac4::strprintf("texture creation failed (%ux%u format %d, %.0f MiB cached): drawn without it",
                                            data.width, data.height, int(data.format), g_tex_bytes / 1048576.0));
  }
  return e.srv;
}

// Depth compare (shadow) fetch (rsx_facts §2.9, as RPCS3): a depth format, a compare function
// other than NEVER/ALWAYS (ADDRESS[31:28]) and alpha kill off. The fetch returns the filtered test
// result in every channel instead of the depth (2D textures; cube shadows not handled).
bool g_zcull_stats = true;  // "gpu zcullstats on|off" (renderer_report_value)
bool g_depth_compare = true;  // "gpu shadow on|off" (A/B: off samples the depth values as before)
bool depth_compare(const gpu::TexDesc& t) {
  u32 zfunc = t.address >> 28;
  return g_depth_compare && t.format >= 0x90 && t.format <= 0x93 && zfunc != 0 && zfunc != 7 && !((t.control0 >> 2) & 1) && !t.cubemap &&
         t.dim == 2;
}

D3D11_SAMPLER_DESC sampler_desc(const gpu::TexDesc& t) {
  D3D11_SAMPLER_DESC sd{};
  u32 minf = (t.filter >> 16) & 7, magf = (t.filter >> 24) & 7;
  bool min_lin = minf == 2 || minf == 4 || minf == 6 || minf == 7, mag_lin = magf != 1;
  bool mip_lin = minf == 5 || minf == 6;
  bool mips = minf >= 3 && minf <= 6;
  u32 aniso = (t.control0 >> 4) & 7;
  if (aniso) {
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    static const u32 kAniso[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    sd.MaxAnisotropy = kAniso[aniso];
  } else {
    sd.Filter = D3D11_FILTER(D3D11_ENCODE_BASIC_FILTER(min_lin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                                       mag_lin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                                       mip_lin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                                       D3D11_FILTER_REDUCTION_TYPE_STANDARD));
    sd.MaxAnisotropy = 1;
  }
  sd.AddressU = wrap_mode(t.address & 15);
  sd.AddressV = wrap_mode((t.address >> 8) & 15);
  sd.AddressW = wrap_mode((t.address >> 16) & 15);
  s32 bias = s32(t.filter << 19) >> 19;  // signed 1.4.8
  sd.MipLODBias = float(bias) / 256.0f;
  sd.MinLOD = float((t.control0 >> 19) & 0xfff) / 256.0f;
  sd.MaxLOD = mips ? float((t.control0 >> 7) & 0xfff) / 256.0f : sd.MinLOD;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  if (depth_compare(t)) {
    // The RSX compares (texel OP reference), D3D (reference OP texel): the order flips.
    static const D3D11_COMPARISON_FUNC kFlipped[8] = {D3D11_COMPARISON_NEVER,         D3D11_COMPARISON_GREATER,
                                                      D3D11_COMPARISON_EQUAL,         D3D11_COMPARISON_GREATER_EQUAL,
                                                      D3D11_COMPARISON_LESS,          D3D11_COMPARISON_NOT_EQUAL,
                                                      D3D11_COMPARISON_LESS_EQUAL,    D3D11_COMPARISON_ALWAYS};
    sd.ComparisonFunc = kFlipped[(t.address >> 28) & 7];
    sd.Filter = mag_lin ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sd.MaxAnisotropy = 1;
  }
  u32 b = t.border;
  sd.BorderColor[0] = ((b >> 16) & 0xff) / 255.0f;
  sd.BorderColor[1] = ((b >> 8) & 0xff) / 255.0f;
  sd.BorderColor[2] = (b & 0xff) / 255.0f;
  sd.BorderColor[3] = (b >> 24) / 255.0f;
  return sd;
}

// ---------------------------------------------------------------------------------------------
// Vertex data and draw clauses
float g_imm[16][4];               // immediate (register) attribute values
std::vector<std::array<float, 64>> g_imm_vertices;  // immediate-mode vertices inside BEGIN/END
u32 g_prim = 0;                   // primitive of the current BEGIN/END block (0 = none)
enum class Clause { None, Arrays, Indexed, Inline, Elements };
Clause g_clause = Clause::None;
std::vector<std::pair<u32, u32>> g_ranges;  // (first, count)
std::vector<u32> g_inline;                  // INLINE_ARRAY words
std::vector<u32> g_elements;                // ARRAY_ELEMENT indices

float half_to_float(u16 h) {
  u32 s = u32(h >> 15) << 31, e = (h >> 10) & 31, mnt = h & 1023, f;
  if (!e) {
    if (!mnt) f = s;
    else {
      e = 113;
      while (!(mnt & 1024)) {
        mnt <<= 1;
        --e;
      }
      f = s | (e << 23) | ((mnt & 1023) << 13);
    }
  } else if (e == 31) {
    f = s | 0x7f800000u | (mnt << 13);
  } else {
    f = s | ((e + 112) << 23) | (mnt << 13);
  }
  return f32_of(f);
}

u32 type_size(u32 type) {
  switch (type) {
    case 1: case 3: case 5: return 2;
    case 2: return 4;
    case 6: return 4;  // whole element
    default: return 1;
  }
}

struct AttrSrc {
  bool array = false;
  u32 type = 0, size = 0, stride = 0, freq = 0, ea = 0;
  bool modulo = false;
};

void attr_sources(AttrSrc* a) {
  u32 base = reg(m::SET_VERTEX_DATA_BASE_OFFSET);
  u32 divop = reg(m::SET_FREQUENCY_DIVIDER_OPERATION);
  for (u32 i = 0; i < 16; ++i) {
    u32 fmt = reg(m::SET_VERTEX_DATA_ARRAY_FORMAT + 4 * i);
    u32 off = reg(m::SET_VERTEX_DATA_ARRAY_OFFSET + 4 * i);
    a[i].type = fmt & 7;
    a[i].size = (fmt >> 4) & 15;
    a[i].stride = (fmt >> 8) & 0xff;
    a[i].freq = fmt >> 16;
    a[i].modulo = (divop >> i) & 1;
    a[i].array = a[i].size != 0;
    if (a[i].array) a[i].ea = offset_to_ea((base + (off & 0x7fffffff)) & 0x0fffffff, off >> 31);
    if (a[i].array && !a[i].ea) a[i].array = false;
  }
}

// ---------------------------------------------------------------------------------------------
// Raw vertex streams (vp_hlsl.h VtxFmt). The guest memory ranges a draw's vertex arrays span are
// copied as they are (big-endian) and the vertex shader decodes each attribute, instead of this
// thread converting every vertex to 16 float4 (256 bytes per vertex, written twice here and copied
// again by the submission thread). Ranges a small gap apart share a stream (interleaved arrays).
// Streams go into a ring buffer, or into a buffer of their own once seen unwritten since an
// earlier frame (static meshes, kept valid by write tracking).
ID3D11Buffer* g_raw = nullptr;
ID3D11ShaderResourceView* g_raw_srv = nullptr;
u32 g_raw_cap = 0, g_raw_pos = 0;  // bytes
ID3D11Buffer* g_ibuf = nullptr;
u32 g_ibuf_cap = 0, g_ibuf_pos = 0;  // indices
ID3D11Buffer* g_vtx_fmt = nullptr;  // VtxFmt constant buffer (b2)

ID3D11ShaderResourceView* raw_view(ID3D11Buffer* b, u32 bytes) {
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = DXGI_FORMAT_R32_TYPELESS;
  sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  sd.BufferEx.FirstElement = 0;
  sd.BufferEx.NumElements = bytes / 4;
  sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ID3D11ShaderResourceView* v = nullptr;
  return SUCCEEDED(device().dev->CreateShaderResourceView(b, &sd, &v)) ? v : nullptr;
}

// Room for `n` bytes in the ring, filled by the caller before the next recorded call; *offset
// receives their position (16-byte aligned). Everything one draw reads from the ring must come
// from one call: a wrap discards what was written before it. Null if the ring is unavailable.
u8* raw_space(u32 n, u32* offset) {
  constexpr u32 kMinCap = 64u << 20;
  n = (n + 15) & ~15u;
  if (n > g_raw_cap || !g_raw) {
    release(g_raw_srv);
    release(g_raw);
    g_raw_cap = std::max({n * 2, g_raw_cap * 2, kMinCap});
    D3D11_BUFFER_DESC bd{g_raw_cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    if (FAILED(device().dev->CreateBuffer(&bd, nullptr, &g_raw)) || !(g_raw_srv = raw_view(g_raw, g_raw_cap))) {
      release(g_raw);
      g_raw_cap = 0;
      return nullptr;
    }
    g_raw_pos = g_raw_cap;  // discard on first use
  }
  D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (g_raw_pos + n > g_raw_cap) {
    mode = D3D11_MAP_WRITE_DISCARD;
    g_raw_pos = 0;
  }
  *offset = g_raw_pos;
  g_raw_pos += n;
  return static_cast<u8*>(gpu::rec().write_buffer_space(g_raw, mode, *offset, n));
}

// Appends `n` indices; returns the start index for the draw.
u32 upload_indices(const u32* idx, u32 n) {
  constexpr u32 kMinCap = 1u << 22;  // 4 M indices = 16 MiB
  if (n > g_ibuf_cap || !g_ibuf) {
    release(g_ibuf);
    g_ibuf_cap = std::max({n * 2, g_ibuf_cap * 2, kMinCap});
    D3D11_BUFFER_DESC bd{g_ibuf_cap * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    device().dev->CreateBuffer(&bd, nullptr, &g_ibuf);
    g_ibuf_pos = g_ibuf_cap;
  }
  D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (g_ibuf_pos + n > g_ibuf_cap) {
    mode = D3D11_MAP_WRITE_DISCARD;
    g_ibuf_pos = 0;
  }
  u32 start = g_ibuf_pos;
  gpu::rec().write_buffer(g_ibuf, mode, start * 4, idx, n * 4);
  g_ibuf_pos += n;
  return start;
}

// Converts the vertex index sequence of RSX primitive `prim` (with ~0u restart markers for strips)
// into a host topology and index list over positions in `seq`.
D3D11_PRIMITIVE_TOPOLOGY convert_primitive(u32 prim, const std::vector<u32>& seq, std::vector<u32>* out) {
  constexpr u32 R = ~0u;
  // Split at restart markers.
  static std::vector<std::pair<u32, u32>> runs;  // reused: called per draw (FIFO thread only)
  runs.clear();
  u32 s = 0;
  for (u32 i = 0; i <= seq.size(); ++i)
    if (i == seq.size() || seq[i] == R) {
      if (i > s) runs.push_back({s, i - s});
      s = i + 1;
    }
  auto emit = [&](u32 p) { out->push_back(p); };
  switch (prim) {
    case 1:  // points
      for (auto [b, n] : runs) for (u32 i = 0; i < n; ++i) emit(b + i);
      return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 2:  // lines
      for (auto [b, n] : runs) for (u32 i = 0; i + 1 < n; i += 2) { emit(b + i); emit(b + i + 1); }
      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3:  // line loop
    case 4:  // line strip
      for (auto [b, n] : runs) {
        for (u32 i = 0; i + 1 < n; ++i) { emit(b + i); emit(b + i + 1); }
        if (prim == 3 && n > 2) { emit(b + n - 1); emit(b); }
      }
      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 5:  // triangles
      for (auto [b, n] : runs) for (u32 i = 0; i + 2 < n; i += 3) { emit(b + i); emit(b + i + 1); emit(b + i + 2); }
      return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 6:  // triangle strip
    case 9:  // quad strip (same vertex order)
      for (auto [b, n] : runs)
        for (u32 i = 0; i + 2 < n; ++i) {
          if (i & 1) { emit(b + i + 1); emit(b + i); emit(b + i + 2); }
          else { emit(b + i); emit(b + i + 1); emit(b + i + 2); }
        }
      return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 7:   // triangle fan
    case 10:  // polygon
      for (auto [b, n] : runs) for (u32 i = 1; i + 1 < n; ++i) { emit(b); emit(b + i); emit(b + i + 1); }
      return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 8:  // quads
      for (auto [b, n] : runs)
        for (u32 i = 0; i + 3 < n; i += 4) {
          emit(b + i); emit(b + i + 1); emit(b + i + 2);
          emit(b + i + 2); emit(b + i + 3); emit(b + i);
        }
      return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:
      return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  }
}

struct VsState {
  float scale[4], offset[4], rt[4];
  u32 flags[4];
};
struct FpState {
  float wpos[4];
  u32 flags[4];
  float alpha[4];
  float fog[4];
  float tex_scale[16][4];
};

// Player builds: no frame reports.
constexpr bool fr_on() { return false; }
void fr_forget(Surface*) {}
void fr_drop(const char*) {}

void fr_frame_boundary() {}

// Whether a draw can change its depth surface under the state set_pipeline_state gives it: depth
// writes need the depth test, stencil writes a nonzero write mask and an operation other than
// KEEP (on either face). Copies of the surface (depth as color) are redone only after such draws.
bool depth_changed(const Targets& t) {
  if (!t.depth) return false;
  if (reg(m::SET_DEPTH_TEST_ENABLE) && reg(m::SET_DEPTH_MASK)) return true;
  if (!reg(m::SET_STENCIL_TEST_ENABLE) || t.depth_format != 2 || !(reg(m::SET_STENCIL_MASK) & 0xff)) return false;
  auto writes = [](u32 fail, u32 zfail, u32 zpass) {
    return stencil_op(reg(fail)) != D3D11_STENCIL_OP_KEEP || stencil_op(reg(zfail)) != D3D11_STENCIL_OP_KEEP ||
           stencil_op(reg(zpass)) != D3D11_STENCIL_OP_KEEP;
  };
  if (writes(m::SET_STENCIL_OP_FAIL, m::SET_STENCIL_OP_ZFAIL, m::SET_STENCIL_OP_ZPASS)) return true;
  return reg(m::SET_TWO_SIDED_STENCIL_TEST_ENABLE) &&
         writes(m::SET_BACK_STENCIL_OP_FAIL, m::SET_BACK_STENCIL_OP_ZFAIL, m::SET_BACK_STENCIL_OP_ZPASS);
}

void set_pipeline_state(const Targets& t) {
  auto* c = &gpu::rec();
  // Blend.
  D3D11_BLEND_DESC bd{};
  bd.IndependentBlendEnable = TRUE;
  u32 sf = reg(m::SET_BLEND_FUNC_SFACTOR), df = reg(m::SET_BLEND_FUNC_DFACTOR), eq = reg(m::SET_BLEND_EQUATION);
  u32 mrt_en = reg(m::SET_BLEND_ENABLE_MRT), cm = reg(m::SET_COLOR_MASK), cm_mrt = reg(m::SET_COLOR_MASK_MRT);
  for (u32 i = 0; i < 4; ++i) {
    auto& rt = bd.RenderTarget[i];
    rt.BlendEnable = i == 0 ? reg(m::SET_BLEND_ENABLE) != 0 : ((mrt_en >> i) & 1) != 0;
    rt.SrcBlend = blend_factor(sf & 0xffff, false);
    rt.DestBlend = blend_factor(df & 0xffff, false);
    rt.BlendOp = blend_op(eq & 0xffff);
    rt.SrcBlendAlpha = blend_factor(sf >> 16, true);
    rt.DestBlendAlpha = blend_factor(df >> 16, true);
    rt.BlendOpAlpha = blend_op(eq >> 16);
    u8 mask;
    if (i == 0)
      mask = UINT8(((cm >> 16) & 1) | (((cm >> 8) & 1) << 1) | ((cm & 1) << 2) | (((cm >> 24) & 1) << 3));
    else {
      u32 b = cm_mrt >> (4 * i);  // +0 A, +1 R, +2 G, +3 B
      mask = UINT8(((b >> 1) & 1) | (((b >> 2) & 1) << 1) | (((b >> 3) & 1) << 2) | ((b & 1) << 3));
    }
    rt.RenderTargetWriteMask = mask;
  }
  if (reg(m::SET_LOGIC_OP_ENABLE)) warn_once("logicop", "logic ops are not implemented (blending used instead)");
  u32 bc = reg(m::SET_BLEND_COLOR);
  float bf[4] = {((bc >> 16) & 0xff) / 255.0f, ((bc >> 8) & 0xff) / 255.0f, (bc & 0xff) / 255.0f, (bc >> 24) / 255.0f};
  if ((sf & 0xffff) == 0x8003 || (df & 0xffff) == 0x8003 || (sf & 0xffff) == 0x8004 || (df & 0xffff) == 0x8004)
    bf[0] = bf[1] = bf[2] = bf[3];  // constant alpha
  ID3D11BlendState* bs = blend_state(bd);
  if (bs != g_sh.blend || std::memcmp(bf, g_sh.blend_factor, sizeof bf)) {
    c->OMSetBlendState(bs, bf, 0xffffffff);
    g_sh.blend = bs;
    std::memcpy(g_sh.blend_factor, bf, sizeof bf);
  }
  // Depth / stencil (depth_changed below follows the same rules).
  D3D11_DEPTH_STENCIL_DESC dd{};
  bool zt = reg(m::SET_DEPTH_TEST_ENABLE) && t.depth;
  dd.DepthEnable = zt;
  dd.DepthWriteMask = zt && reg(m::SET_DEPTH_MASK) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.DepthFunc = cmp_func(reg(m::SET_DEPTH_FUNC));
  bool st = reg(m::SET_STENCIL_TEST_ENABLE) && t.depth && t.depth_format == 2;
  dd.StencilEnable = st;
  dd.StencilReadMask = UINT8(reg(m::SET_STENCIL_FUNC_MASK));
  dd.StencilWriteMask = UINT8(reg(m::SET_STENCIL_MASK));
  dd.FrontFace = {stencil_op(reg(m::SET_STENCIL_OP_FAIL)), stencil_op(reg(m::SET_STENCIL_OP_ZFAIL)),
                  stencil_op(reg(m::SET_STENCIL_OP_ZPASS)), cmp_func(reg(m::SET_STENCIL_FUNC))};
  if (reg(m::SET_TWO_SIDED_STENCIL_TEST_ENABLE))
    dd.BackFace = {stencil_op(reg(m::SET_BACK_STENCIL_OP_FAIL)), stencil_op(reg(m::SET_BACK_STENCIL_OP_ZFAIL)),
                   stencil_op(reg(m::SET_BACK_STENCIL_OP_ZPASS)), cmp_func(reg(m::SET_BACK_STENCIL_FUNC))};
  else
    dd.BackFace = dd.FrontFace;
  ID3D11DepthStencilState* ds = depth_state(dd);
  u32 ref = reg(m::SET_STENCIL_FUNC_REF) & 0xff;
  if (ds != g_sh.depth || ref != g_sh.stencil_ref) {
    c->OMSetDepthStencilState(ds, ref);
    g_sh.depth = ds;
    g_sh.stencil_ref = ref;
  }
  // Rasterizer.
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = reg(m::SET_FRONT_POLYGON_MODE) == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  if (reg(m::SET_CULL_FACE_ENABLE)) {
    u32 cf = reg(m::SET_CULL_FACE);
    rd.CullMode = cf == 0x0404 ? D3D11_CULL_FRONT : cf == 0x0405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
  }
  rd.FrontCounterClockwise = reg(m::SET_FRONT_FACE) == 0x0901;
  rd.DepthClipEnable = (reg(m::SET_ZMIN_MAX_CONTROL) & 0xf) != 0;
  rd.ScissorEnable = TRUE;
  if (reg(m::SET_POLY_OFFSET_FILL_ENABLE)) {
    rd.DepthBias = INT(f32_of(reg(m::SET_POLYGON_OFFSET_BIAS)));
    // The slope is measured per storage pixel: at render scale f a pixel spans 1/f of the game's,
    // so the factor grows by f for the same offset in depth (shadow maps keep their bias).
    float f = std::max(t.fx / float(t.sx), t.fy / float(t.sy));
    rd.SlopeScaledDepthBias = f32_of(reg(m::SET_POLYGON_OFFSET_SCALE_FACTOR)) * f;
  }
  ID3D11RasterizerState* rs = raster_state(rd);
  if (rs != g_sh.raster) {
    c->RSSetState(rs);
    g_sh.raster = rs;
  }
  D3D11_RECT sr = scissor_rect(t, false);
  if (std::memcmp(&sr, &g_sh.scissor, sizeof sr)) {
    c->RSSetScissorRects(1, &sr);
    g_sh.scissor = sr;
  }
}

// ---------------------------------------------------------------------------------------------
// Static vertex data: streams and converted index lists kept in buffers of their own while write
// tracking reports no write to the guest memory they came from. A candidate (no buffer yet) is
// recorded when first seen and stored when seen again in a later frame without writes in between;
// data rewritten every frame keeps going through the rings (buffer creation is costly).
struct StreamEntry {
  ID3D11Buffer* buf = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  u32 serial = 0, bytes = 0;
  u64 last_frame = 0, seen_frame = ~0ull;
};
struct IndexEntry {
  ID3D11Buffer* ib = nullptr;
  u32 count = 0, lo = 0, hi = 0, serial = 0, bytes = 0;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  u64 last_frame = 0, seen_frame = ~0ull;
  std::vector<std::pair<u32, u32>> ranges;  // index memory read: [ea, ea + size)
};
std::unordered_map<u64, StreamEntry> g_streams;     // key: ea << 32 | size
std::unordered_map<u64, IndexEntry> g_index_lists;  // key: hash of the index source and primitive
u64 g_vdata_bytes = 0;
constexpr u64 kVdataBudget = 512ull << 20;
constexpr u32 kStreamCacheMin = 16u << 10;  // bytes; smaller streams always go through the ring
constexpr u32 kIndexCacheMin = 256;         // indices

void vdata_release(StreamEntry& e) {
  release(e.srv);
  release(e.buf);
  g_vdata_bytes -= e.bytes;
  e.bytes = 0;
}
void vdata_release(IndexEntry& e) {
  release(e.ib);
  g_vdata_bytes -= e.bytes;
  e.bytes = 0;
}
// Once over the byte budget or the entry limit: drops entries unused for 120 frames and
// candidates unused for two (at frame boundaries; while everything is in use, once a second).
void vdata_trim() {
  static u64 next_scan = 0;
  if ((g_vdata_bytes <= kVdataBudget && g_streams.size() + g_index_lists.size() < 50000) || g_frame < next_scan) return;
  next_scan = g_frame + 60;
  auto trim = [](auto& map) {
    for (auto it = map.begin(); it != map.end();) {
      if (it->second.last_frame + 120 < g_frame || (!it->second.bytes && it->second.last_frame + 2 < g_frame)) {
        vdata_release(it->second);
        it = map.erase(it);
      } else {
        ++it;
      }
    }
  };
  trim(g_streams);
  trim(g_index_lists);
}

// The buffer of a stream that stayed unwritten since an earlier frame (created now if needed), or
// null: the stream goes through the ring this time.
StreamEntry* stream_cached(u32 ea, u32 size) {
  StreamEntry& e = g_streams[(u64(ea) << 32) | size];
  e.last_frame = g_frame;
  if (e.buf) {
    if (g_track_writes && !written_since(ea, size, e.serial)) return &e;
    vdata_release(e);  // rewritten: a candidate again
    e.seen_frame = g_frame;
    e.serial = g_mem_serial;
    watch_range(ea, size);
    return nullptr;
  }
  if (e.seen_frame == g_frame) return nullptr;
  if (e.seen_frame == ~0ull || !g_track_writes || written_since(ea, size, e.serial)) {
    e.seen_frame = g_frame;
    e.serial = g_mem_serial;
    watch_range(ea, size);
    return nullptr;
  }
  D3D11_BUFFER_DESC bd{size, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
  D3D11_SUBRESOURCE_DATA init{::ppu::mem() + ea, 0, 0};
  if (FAILED(device().dev->CreateBuffer(&bd, &init, &e.buf)) || !(e.srv = raw_view(e.buf, size))) {
    release(e.buf);
    return nullptr;
  }
  e.bytes = size;
  g_vdata_bytes += size;
  e.serial = g_mem_serial;
  watch_range(ea, size);
  return &e;
}

// The draw's vertex data and indices, ready to bind.
struct DrawGeom {
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11Buffer* ib = nullptr;  // null: Draw(count, start) without indices (start = first element)
  u32 count = 0, start = 0;
  ID3D11ShaderResourceView* srv[gpu::kVtxBuffers] = {};
  gpu::VtxFmt fmt{};
  u32 lo = 0, hi = 0;   // element index range
  bool cached = true;   // nothing went through the rings
  bool inline_data = false;
};

// Element index sequence of an array, indexed or element draw (~0u: primitive restart).
void draw_sequence(Clause clause, std::vector<u32>& seq) {
  seq.clear();
  if (clause == Clause::Arrays) {
    for (auto [first, count] : g_ranges)
      for (u32 i = 0; i < count; ++i) seq.push_back(first + i);
  } else if (clause == Clause::Indexed) {
    bool restart = reg(m::SET_RESTART_INDEX_ENABLE) != 0;
    u32 restart_value = reg(m::SET_RESTART_INDEX);
    u32 dma = reg(m::SET_INDEX_ARRAY_DMA);
    bool u16i = ((dma >> 4) & 15) == 1;
    u32 isz = u16i ? 2 : 4;
    u32 base_index = reg(m::SET_VERTEX_DATA_BASE_INDEX);
    u32 iaddr = reg(m::SET_INDEX_ARRAY_ADDRESS) & ~(isz - 1);
    for (auto [first, count] : g_ranges) {
      u32 ea = offset_to_ea(iaddr + first * isz, dma & 15);
      if (!ea) continue;
      const u8* p = ::ppu::mem() + ea;
      for (u32 i = 0; i < count; ++i) {
        u32 v;
        if (u16i) {
          u16 h;
          std::memcpy(&h, p + 2 * i, 2);
          v = __builtin_bswap16(h);
        } else {
          std::memcpy(&v, p + 4 * i, 4);
          v = __builtin_bswap32(v);
        }
        if (restart && v == restart_value) {
          seq.push_back(~0u);
          continue;
        }
        seq.push_back((v + base_index) & 0xfffff);
      }
    }
  } else if (clause == Clause::Elements) {
    seq.assign(g_elements.begin(), g_elements.end());
  }
}

// Converts element index sequence `seq` for primitive `prim` and uploads the index list to the ring.
bool upload_sequence(u32 prim, const std::vector<u32>& seq, DrawGeom* g) {
  static std::vector<u32> positions, indices;
  // Triangle lists without restart markers (most draws): the sequence is the index list.
  if (prim == 5 && seq.size() >= 3 && std::find(seq.begin(), seq.end(), ~0u) == seq.end()) {
    g->topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    g->count = u32(seq.size() - seq.size() % 3);
    int prev = phase_switch(kPhUploads);
    g->start = upload_indices(seq.data(), g->count);
    phase_switch(prev);
    g->ib = g_ibuf;
    g->cached = false;
    return true;
  }
  positions.clear();
  g->topo = convert_primitive(prim, seq, &positions);
  if (g->topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED || positions.empty()) {
    fr_drop("primitive not drawable");
    return false;
  }
  indices.clear();
  indices.reserve(positions.size());
  for (u32 p : positions) indices.push_back(seq[p]);
  g->count = u32(indices.size());
  int prev = phase_switch(kPhUploads);
  g->start = upload_indices(indices.data(), g->count);
  phase_switch(prev);
  g->ib = g_ibuf;
  g->cached = false;
  return true;
}

// Index list of an array, indexed or element draw: none for a list drawn from one array range,
// else a cached list or one converted now. Sets the element index range.
bool draw_indices(Clause clause, u32 prim, DrawGeom* g) {
  if (clause == Clause::Arrays && g_ranges.size() == 1 && (prim == 1 || prim == 2 || prim == 5)) {
    g->topo = prim == 1 ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
              : prim == 2 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                          : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    g->ib = nullptr;
    g->start = g_ranges[0].first;
    g->count = g_ranges[0].second;
    g->lo = g->start;
    g->hi = g->start + g->count - 1;
    return true;
  }
  u32 total = 0;
  for (auto [first, count] : g_ranges) total += count;
  bool cacheable = (clause == Clause::Arrays || clause == Clause::Indexed) && total >= kIndexCacheMin;
  IndexEntry* e = nullptr;
  u64 key = 0;
  if (cacheable) {
    u32 fixed[8] = {u32(clause), prim, 0, 0, 0, 0, 0, 0};
    if (clause == Clause::Indexed) {
      fixed[2] = reg(m::SET_INDEX_ARRAY_DMA);
      fixed[3] = reg(m::SET_INDEX_ARRAY_ADDRESS);
      fixed[4] = reg(m::SET_VERTEX_DATA_BASE_INDEX);
      fixed[5] = reg(m::SET_RESTART_INDEX_ENABLE);
      fixed[6] = reg(m::SET_RESTART_INDEX);
    }
    key = hash_bytes(reinterpret_cast<const u8*>(g_ranges.data()), g_ranges.size() * sizeof(g_ranges[0])) ^
          hash_bytes(reinterpret_cast<const u8*>(fixed), sizeof fixed) * 31;
    e = &g_index_lists[key];
    e->last_frame = g_frame;
    bool valid = e->ib && g_track_writes;
    for (size_t r = 0; valid && r < e->ranges.size(); ++r) valid = !written_since(e->ranges[r].first, e->ranges[r].second, e->serial);
    if (valid) {
      g->topo = e->topo;
      g->ib = e->ib;
      g->count = e->count;
      g->start = 0;
      g->lo = e->lo;
      g->hi = e->hi;
      return true;
    }
  }
  static std::vector<u32> seq;
  draw_sequence(clause, seq);
  u32 lo = ~0u, hi = 0;
  for (u32 k : seq)
    if (k != ~0u) {
      lo = std::min(lo, k);
      hi = std::max(hi, k);
    }
  if (lo == ~0u) {
    fr_drop("no vertices");
    return false;
  }
  g->lo = lo;
  g->hi = hi;
  if (!cacheable) return upload_sequence(prim, seq, g);
  e->ranges.clear();
  if (clause == Clause::Indexed) {
    u32 dma = reg(m::SET_INDEX_ARRAY_DMA);
    u32 isz = ((dma >> 4) & 15) == 1 ? 2 : 4;
    u32 iaddr = reg(m::SET_INDEX_ARRAY_ADDRESS) & ~(isz - 1);
    for (auto [first, count] : g_ranges)
      if (u32 ea = offset_to_ea(iaddr + first * isz, dma & 15)) e->ranges.push_back({ea, count * isz});
  }
  bool unchanged = e->seen_frame != ~0ull && e->seen_frame != g_frame && !e->ib && g_track_writes;
  for (size_t r = 0; unchanged && r < e->ranges.size(); ++r)
    unchanged = !written_since(e->ranges[r].first, e->ranges[r].second, e->serial);
  if (!unchanged) {  // first sight or rewritten: a candidate (again)
    if (e->seen_frame != g_frame || e->ib) {
      vdata_release(*e);
      e->seen_frame = g_frame;
      e->serial = g_mem_serial;
      for (auto [ea, size] : e->ranges) watch_range(ea, size);
    }
    return upload_sequence(prim, seq, g);
  }
  // Unwritten since an earlier frame: a buffer of its own.
  static std::vector<u32> positions, indices;
  positions.clear();
  D3D11_PRIMITIVE_TOPOLOGY topo = convert_primitive(prim, seq, &positions);
  if (topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED || positions.empty()) {
    fr_drop("primitive not drawable");
    return false;
  }
  indices.clear();
  indices.reserve(positions.size());
  for (u32 p : positions) indices.push_back(seq[p]);
  D3D11_BUFFER_DESC id{u32(indices.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA ii{indices.data(), 0, 0};
  if (FAILED(device().dev->CreateBuffer(&id, &ii, &e->ib))) {
    release(e->ib);
    return upload_sequence(prim, seq, g);
  }
  e->count = u32(indices.size());
  e->topo = topo;
  e->lo = lo;
  e->hi = hi;
  e->bytes = id.ByteWidth;
  g_vdata_bytes += e->bytes;
  e->serial = g_mem_serial;
  for (auto [ea, size] : e->ranges) watch_range(ea, size);
  g->topo = topo;
  g->ib = e->ib;
  g->count = e->count;
  g->start = 0;
  return true;
}

// Vertex streams of an array, indexed or element draw for the inputs the program reads: attribute
// descriptors and buffer bindings.
bool draw_streams(const AttrSrc* a, u32 mask, DrawGeom* g) {
  struct Range {
    u32 b, e, attrs;  // bytes [b, e) and the attributes reading them
  };
  Range rs[16];
  u32 nr = 0;
  for (u32 i = 0; i < 16; ++i) {
    if (!((mask >> i) & 1)) continue;
    if (!a[i].array) {
      std::memcpy(g->fmt.konst[i], g_imm[i], 16);
      continue;
    }
    u32 elo = g->lo, ehi = g->hi;
    if (a[i].stride == 0) {
      elo = ehi = 0;
    } else if (a[i].freq > 1 && a[i].modulo) {
      elo = 0;
      ehi = std::min(g->hi, a[i].freq - 1);
    } else if (a[i].freq > 1) {
      elo = g->lo / a[i].freq;
      ehi = g->hi / a[i].freq;
    }
    u32 nb = a[i].type == 6 ? 4 : std::min(a[i].size, 4u) * type_size(a[i].type);
    u64 b = u64(a[i].ea) + u64(elo) * a[i].stride, e = u64(a[i].ea) + u64(ehi) * a[i].stride + nb;
    if (e >= 0x100000000ull || e - b > (32u << 20)) {
      fr_drop("vertex array range outside guest memory or over 32 MiB");
      return false;
    }
    g->fmt.attr[i] = {0, a[i].stride, a[i].freq, a[i].type | (std::min(a[i].size, 4u) << 4) | (u32(a[i].modulo) << 12) | (1u << 16)};
    rs[nr++] = {u32(b), u32(e), 1u << i};
  }
  // Streams: ranges merged when they overlap or lie up to 256 bytes apart (interleaved arrays).
  std::sort(rs, rs + nr, [](const Range& x, const Range& y) { return x.b < y.b; });
  Range st[16];
  u32 ns = 0;
  for (u32 r = 0; r < nr; ++r) {
    if (ns && rs[r].b <= st[ns - 1].e + 256) {
      st[ns - 1].e = std::max(st[ns - 1].e, rs[r].e);
      st[ns - 1].attrs |= rs[r].attrs;
    } else {
      st[ns++] = rs[r];
    }
  }
  // Large streams from their own buffers (slots 1..3) when cached; the others from the ring (slot
  // 0), all reserved at once.
  u32 slot_of[16], pos_of[16], ring_bytes = 0, next_slot = 1;
  for (u32 s = 0; s < ns; ++s) {
    st[s].b &= ~3u;
    st[s].e = (st[s].e + 3) & ~3u;
    u32 size = st[s].e - st[s].b;
    slot_of[s] = 0;
    if (size >= kStreamCacheMin && next_slot < gpu::kVtxBuffers)
      if (StreamEntry* c = stream_cached(st[s].b, size)) {
        slot_of[s] = next_slot++;
        pos_of[s] = 0;
        g->srv[slot_of[s]] = c->srv;
        continue;
      }
    pos_of[s] = ring_bytes;
    ring_bytes += (size + 15) & ~15u;
  }
  if (ring_bytes) {
    int prev = phase_switch(kPhUploads);
    u32 base;
    u8* dst = raw_space(ring_bytes, &base);
    if (!dst) {
      phase_switch(prev);
      fr_drop("vertex ring unavailable");
      return false;
    }
    for (u32 s = 0; s < ns; ++s)
      if (slot_of[s] == 0) {
        std::memcpy(dst + pos_of[s], ::ppu::mem() + st[s].b, st[s].e - st[s].b);
        pos_of[s] += base;
      }
    phase_switch(prev);
    g->srv[0] = g_raw_srv;
    g->cached = false;
    g_rs.vbytes += ring_bytes;
  }
  for (u32 s = 0; s < ns; ++s)
    for (u32 i = 0; i < 16; ++i)
      if ((st[s].attrs >> i) & 1) {
        g->fmt.attr[i].base = pos_of[s] + (a[i].ea - st[s].b);  // wraps when element 0 lies before the stream
        g->fmt.attr[i].fmt |= slot_of[s] << 8;
      }
  return true;
}

// Inline arrays (INLINE_ARRAY: interleaved vertices in the command stream, each array padded to
// 32-bit words) and immediate-mode vertices (16 float4 each, F32): one ring stream in guest
// layout (big-endian), elements 0..n-1.
bool draw_immediate(const AttrSrc* a, u32 mask, u32 prim, Clause clause, DrawGeom* g) {
  u32 nv, base;
  if (clause == Clause::Inline) {
    u32 stride = 0, offs[16] = {};
    for (u32 i = 0; i < 16; ++i)
      if (a[i].size) {
        offs[i] = stride;
        u32 nb = a[i].type == 6 ? 4 : std::min(a[i].size, 4u) * type_size(a[i].type);
        stride += (nb + 3) & ~3u;
      }
    if (!stride) {
      fr_drop("inline array without enabled arrays");
      return false;
    }
    nv = u32(g_inline.size() * 4 / stride);
    if (!nv) {
      fr_drop("no vertices");
      return false;
    }
    u8* dst = raw_space(u32(g_inline.size() * 4), &base);
    if (!dst) {
      fr_drop("vertex ring unavailable");
      return false;
    }
    for (size_t w = 0; w < g_inline.size(); ++w) {
      u32 be = __builtin_bswap32(g_inline[w]);
      std::memcpy(dst + 4 * w, &be, 4);
    }
    for (u32 i = 0; i < 16; ++i) {
      if (!((mask >> i) & 1)) continue;
      if (a[i].size) g->fmt.attr[i] = {base + offs[i], stride, 0, a[i].type | (std::min(a[i].size, 4u) << 4) | (1u << 16)};
      else std::memcpy(g->fmt.konst[i], g_imm[i], 16);
    }
  } else {
    nv = u32(g_imm_vertices.size());
    u8* dst = raw_space(nv * 256, &base);
    if (!dst) {
      fr_drop("vertex ring unavailable");
      return false;
    }
    for (u32 v = 0; v < nv; ++v)
      for (u32 f = 0; f < 64; ++f) {
        u32 w;
        std::memcpy(&w, &g_imm_vertices[v][f], 4);
        w = __builtin_bswap32(w);
        std::memcpy(dst + v * 256 + 4 * f, &w, 4);
      }
    for (u32 i = 0; i < 16; ++i)
      if ((mask >> i) & 1) g->fmt.attr[i] = {base + 16 * i, 256, 0, 2u | (4u << 4) | (1u << 16)};
  }
  g->srv[0] = g_raw_srv;
  g->inline_data = true;
  g->lo = 0;
  g->hi = nv - 1;
  static std::vector<u32> seq;
  seq.clear();
  for (u32 v = 0; v < nv; ++v) seq.push_back(v);
  return upload_sequence(prim, seq, g);
}

void flush_draw() {
  bool have = !g_ranges.empty() || !g_inline.empty() || !g_elements.empty() || !g_imm_vertices.empty();
  u32 prim = g_prim;
  Clause clause = g_clause;
  if (!have || !g_have_device) return;
  PhaseScope phase_scope(kPhSurfaces);
  bump(g.draws);
  if (reg(m::SET_RENDER_ENABLE) >> 24 == 2) {
    u32 ea = kReportAddr + (reg(m::SET_RENDER_ENABLE) & 0xffffff);
    ++g_rs.cond_draws;
    if (ppu::r32(ea + 8) == 0) {  // conditional rendering: report value is zero
      ++g_rs.cond_skipped;
      return;
    }
  }
  // With depth and stencil tests disabled a draw neither reads nor writes the depth buffer (GL
  // semantics): it is not bound, so passes with other clip sizes leave the depth surface alone.
  Targets t = bind_targets(reg(m::SET_DEPTH_TEST_ENABLE) || reg(m::SET_STENCIL_TEST_ENABLE));
  if (!t.valid) return fr_drop("no valid render target");
  phase_switch(kPhShaders);
  const VsEntry* vs = vertex_shader();
  if (!vs->vs) return fr_drop("vertex program unusable");
  // Fragment program.
  u32 sp = reg(m::SET_SHADER_PROGRAM);
  if (cfg().skip_fp && sp == cfg().skip_fp) return fr_drop("skipped (--skip-fp)");
  u32 fp_ea = offset_to_ea(sp & ~3u, (sp & 3) == 2 ? 1 : 0);
  if (!fp_ea) return fr_drop("fragment program address invalid");
  u64 code_hash = 0;
  static std::vector<u32> const_offsets;
  const_offsets.clear();
  u32 max_bytes = 0x10000;
  u32 fp_size = gpu::fp_scan(::ppu::mem() + fp_ea, max_bytes, &code_hash, &const_offsets);
  if (!fp_size) {
    warn_once("fp-scan", "fragment program without END");
    return fr_drop("fragment program without END");
  }
  gpu::FpEnv env;
  u32 sc = reg(m::SET_SHADER_CONTROL);
  env.fp32_exports = (sc >> 6) & 1;
  env.depth_export = (sc & 0xe) != 0 && t.depth;
  env.mrt_count = u8(std::max<u32>(t.count, 1));
  // Texture units decoded again only when one of their registers changed.
  static gpu::TexDesc tex[16];
  static u32 tex_regs[16][9];
  static bool tex_memo = false;
  for (u32 u = 0; u < 16; ++u) {
    u32 now[9];
    std::memcpy(now, &g.regs[(0x1A00 + 0x20 * u) >> 2], 32);
    now[8] = g.regs[(0x1840 + 4 * u) >> 2];
    if (!tex_memo || std::memcmp(now, tex_regs[u], sizeof now)) {
      tex[u] = gpu::decode_texture(g.regs, u);
      std::memcpy(tex_regs[u], now, sizeof now);
    }
    if (!tex[u].enabled) continue;
    env.tex_dim[u] = u8(depth_compare(tex[u]) ? 5 : tex[u].cubemap ? 4 : tex[u].dim == 3 ? 3 : tex[u].dim == 1 ? 1 : 2);
  }
  tex_memo = true;
  const PsEntry* ps = pixel_shader(env, ::ppu::mem() + fp_ea, max_bytes, code_hash, sp);
  if (!ps->ps) return fr_drop("fragment program unusable");

  // Vertex data and indices.
  phase_switch(kPhVertices);
  AttrSrc a[16];
  attr_sources(a);
  u32 mask = vs->info.input_mask;
  static DrawGeom geo;
  geo = DrawGeom{};
  collect_writes();
  if (clause == Clause::Inline || !g_imm_vertices.empty()) {
    if (!draw_immediate(a, mask, prim, clause, &geo)) return;
  } else if (!draw_indices(clause, prim, &geo) || !draw_streams(a, mask, &geo)) {
    return;
  }
  if (geo.cached) ++g_rs.vcached;
  else ++g_rs.vconverted;
  u32 nverts = geo.hi - geo.lo + 1;
  g_rs.vertices += nverts;
  g_rs.max_draw_vertices = std::max<u64>(g_rs.max_draw_vertices, nverts);

  phase_switch(kPhState);
  auto* c = &gpu::rec();
  set_pipeline_state(t);
  phase_switch(kPhConstants);
  // Constant buffers.
  static bool fmt_valid = false, fc_valid = false;
  const u32 patch = g_patch_cb;
  if (g_cb_reset) {
    g_vc_whole = g_vc_dirty = true;
    fmt_valid = fc_valid = false;
    g_cb_reset = false;
  }
  if (g_vc_dirty) {
    if ((patch & 1) && !g_vc_whole && g_vc_lo <= g_vc_hi) {
      c->UpdateConstants(g_vc_buf, g_vc_lo * 16, &g_vc[g_vc_lo * 4], (g_vc_hi + 1 - g_vc_lo) * 16, sizeof g_vc);
    } else {
      c->UpdateConstants(g_vc_buf, 0, g_vc, sizeof g_vc, sizeof g_vc);
      g_vc_whole = false;
    }
    g_vc_dirty = false;
    g_vc_lo = ~0u;
    g_vc_hi = 0;
  }
  VsState vst{};
  for (int i = 0; i < 4; ++i) {
    vst.scale[i] = f32_of(reg(m::SET_VIEWPORT_SCALE + 4 * i));
    vst.offset[i] = f32_of(reg(m::SET_VIEWPORT_OFFSET + 4 * i));
  }
  vst.rt[0] = float(t.width);
  vst.rt[1] = float(t.height);
  vst.rt[2] = 1.0f / float(t.width);
  vst.rt[3] = 1.0f / float(t.height);
  vst.flags[0] = reg(m::SET_TRANSFORM_BRANCH_BITS);
  static VsState vst_last{};
  static bool vst_valid = false;
  if (!vst_valid || std::memcmp(&vst, &vst_last, sizeof vst)) {
    c->UpdateSubresource(g_vs_state, 0, nullptr, &vst, 0, 0, sizeof vst);
    vst_last = vst;
    vst_valid = true;
  }
  static gpu::VtxFmt fmt_last{};
  if (!fmt_valid || std::memcmp(&geo.fmt, &fmt_last, sizeof fmt_last)) {
    if (fmt_valid && (patch & 4)) {  // the span of changed 16-byte rows
      const u8* cur = reinterpret_cast<const u8*>(&geo.fmt);
      u8* old = reinterpret_cast<u8*>(&fmt_last);
      u32 lo = sizeof fmt_last / 16, hi = 0;
      for (u32 r = 0; r < sizeof fmt_last / 16; ++r)
        if (std::memcmp(cur + 16 * r, old + 16 * r, 16)) {
          lo = std::min(lo, r);
          hi = r + 1;
        }
      c->UpdateConstants(g_vtx_fmt, lo * 16, cur + 16 * lo, (hi - lo) * 16, sizeof geo.fmt);
      std::memcpy(old + 16 * lo, cur + 16 * lo, size_t(hi - lo) * 16);
    } else {
      c->UpdateConstants(g_vtx_fmt, 0, &geo.fmt, sizeof geo.fmt, sizeof geo.fmt);
      fmt_last = geo.fmt;
    }
    fmt_valid = true;
  }
  FpState fst{};
  u32 win = reg(m::SET_SHADER_WINDOW);
  fst.wpos[0] = float(win & 0xfff);
  fst.wpos[1] = float((win >> 12) & 0xf);
  fst.wpos[2] = float((win >> 16) & 0xf);
  fst.wpos[3] = reg(m::SET_TWO_SIDE_LIGHT_EN) ? 1.0f : 0.0f;
  u32 tc2d = 0;
  for (u32 i = 0; i < 10; ++i) tc2d |= (reg(m::SET_TEX_COORD_CONTROL + 4 * i) & 1) << i;
  fst.flags[0] = tc2d;
  bool float_rt = t.color_format >= 11 && t.color_format <= 13;
  fst.flags[1] = reg(m::SET_ALPHA_TEST_ENABLE) && !float_rt ? (reg(m::SET_ALPHA_FUNC) & 7) + 1 : 0;
  fst.alpha[0] = (reg(m::SET_ALPHA_REF) & 0xff) / 255.0f;
  fst.alpha[2] = 1.0f / t.fx;  // window position: storage pixels -> the game's pixels
  fst.alpha[3] = 1.0f / t.fy;
  if (t.color_format == 11) fst.alpha[0] = half_to_float(u16(reg(m::SET_ALPHA_REF)));
  u32 fog_mode = reg(m::SET_FOG_MODE);
  std::memcpy(&fst.fog[0], &fog_mode, 4);
  fst.fog[1] = f32_of(reg(m::SET_FOG_PARAMS));
  fst.fog[2] = f32_of(reg(m::SET_FOG_PARAMS + 4));
  // Textures.
  phase_switch(kPhTextures);
  ID3D11ShaderResourceView* srvs[16] = {};
  ID3D11SamplerState* smps[16] = {};
  for (u32 u = 0; u < 16; ++u) {
    if (!((ps->info.tex_mask >> u) & 1) || !tex[u].enabled) continue;
    srvs[u] = texture_view(tex[u], fst.tex_scale[u]);
    smps[u] = sampler_state(sampler_desc(tex[u]));
    for (u32 i = 0; i < t.count; ++i)
      if (t.color[i]->srv == srvs[u]) srvs[u] = feedback_copy(t.color[i]);  // reads see the target before this draw
    if (t.depth && srvs[u] && t.depth->srv == srvs[u]) srvs[u] = feedback_copy(t.depth);
  }
  phase_switch(kPhConstants);
  static FpState fst_last{};
  static bool fst_valid = false;
  if (!fst_valid || std::memcmp(&fst, &fst_last, sizeof fst)) {
    c->UpdateSubresource(g_fp_state, 0, nullptr, &fst, 0, 0, sizeof fst);
    fst_last = fst;
    fst_valid = true;
  }
  // Fragment constants from the ucode (patched by games in memory).
  static float fc[256 * 4];
  if (!const_offsets.empty()) {
    u32 n = std::min<u32>(u32(const_offsets.size()), 256);
    u32 base = 0;  // offsets are relative to the first non-NOP instruction: find it again
    for (; base + 16 <= max_bytes && ((gpu::fp_word(::ppu::mem() + fp_ea + base) >> 24) & 0x3f) == 0; base += 16) {
    }
    for (u32 i = 0; i < n; ++i)
      for (u32 k = 0; k < 4; ++k) {
        u32 wv = gpu::fp_word(::ppu::mem() + fp_ea + base + const_offsets[i] + 4 * k);
        fc[4 * i + k] = f32_of(wv);
      }
    static float fc_last[256 * 4];
    if (!fc_valid || std::memcmp(fc, fc_last, size_t(n) * 16)) {
      if (fc_valid && (patch & 2)) {  // the program reads c[0..n-1]; fc_last mirrors the buffer
        c->UpdateConstants(g_fp_const, 0, fc, n * 16, sizeof fc);
        std::memcpy(fc_last, fc, size_t(n) * 16);
      } else {
        c->UpdateConstants(g_fp_const, 0, fc, sizeof fc, sizeof fc);
        std::memcpy(fc_last, fc, sizeof fc);
      }
      fc_valid = true;
    }
  }
  phase_switch(kPhDraws);
  if (!g_sh.fixed) {
    ID3D11Buffer* vcb[3] = {g_vc_buf, g_vs_state, g_vtx_fmt};
    c->VSSetConstantBuffers(0, 3, vcb);
    ID3D11Buffer* pcb[2] = {g_fp_state, g_fp_const};
    c->PSSetConstantBuffers(2, 2, pcb);
    c->IASetInputLayout(nullptr);
    g_sh.fixed = 1;
  }
  bind_changed<gpu::kVtxBuffers>(geo.srv, g_sh.vs_srv, [&](u32 first, u32 n) { c->VSSetShaderResources(first, n, geo.srv + first); });
  bind_changed(srvs, g_sh.ps_srv, [&](u32 first, u32 n) { c->PSSetShaderResources(first, n, srvs + first); });
  bind_changed(smps, g_sh.smp, [&](u32 first, u32 n) { c->PSSetSamplers(first, n, smps + first); });
  if (g_sh.vs != vs->vs) {
    c->VSSetShader(vs->vs, nullptr, 0);
    g_sh.vs = vs->vs;
  }
  if (g_sh.ps != ps->ps) {
    c->PSSetShader(ps->ps, nullptr, 0);
    g_sh.ps = ps->ps;
  }
  if (g_sh.topo != u32(geo.topo)) {
    c->IASetPrimitiveTopology(geo.topo);
    g_sh.topo = u32(geo.topo);
  }
  if (geo.ib && g_sh.ib != geo.ib) {
    c->IASetIndexBuffer(geo.ib, DXGI_FORMAT_R32_UINT, 0);
    g_sh.ib = geo.ib;
  }
  if (gpu::timing::per_draw())
    gpu::timing::mark(ac4::strprintf("%s fp %08x vp %08x", g_rt_label.c_str(), u32(code_hash), u32(reinterpret_cast<uintptr_t>(vs->vs) >> 4)));
  if (geo.ib) c->DrawIndexed(geo.count, geo.start, 0);
  else c->Draw(geo.count, geo.start);
  if (gpu::timing::per_draw()) gpu::timing::mark(g_rt_label + " between draws");
  gpu::rec().kick();
  ++g_rs.draws;
  ++g_write_serial;
  for (u32 i = 0; i < t.count; ++i) t.color[i]->written = g_write_serial;
  if (depth_changed(t)) t.depth->written = g_write_serial;
}

void fr_clear(const Targets&, u32) {}

void reset_clause() {
  g_ranges.clear();
  g_inline.clear();
  g_elements.clear();
  g_imm_vertices.clear();
  g_clause = Clause::None;
}

void set_immediate(u32 attr, u32 comp_mask, const float* v) {
  for (u32 c = 0; c < 4; ++c)
    if (comp_mask & (1u << c)) g_imm[attr][c] = v[c];
  // Immediate-mode vertex emission: completing attribute 0 inside BEGIN/END emits a vertex.
  if (g_prim && attr == 0 && g_clause == Clause::None) {
    std::array<float, 64> vtx;
    std::memcpy(vtx.data(), g_imm, sizeof vtx);
    g_imm_vertices.push_back(vtx);
  }
}

void maybe_capture() {}  // player builds: no test captures

// Guest-memory presentation (when no surface was rendered at the display buffer).
ID3D11Texture2D* g_mem_tex = nullptr;
ID3D11ShaderResourceView* g_mem_srv = nullptr;
u32 g_mem_w = 0, g_mem_h = 0;

void present_memory(const DisplayBuffer& b) {
  u32 ea = offset_to_ea(b.offset, 0);
  if (!ea) return;
  if (!g_mem_tex || g_mem_w != b.width || g_mem_h != b.height) {
    release(g_mem_srv);
    release(g_mem_tex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = b.width;
    td.Height = b.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device().dev->CreateTexture2D(&td, nullptr, &g_mem_tex);
    device().dev->CreateShaderResourceView(g_mem_tex, nullptr, &g_mem_srv);
    g_mem_w = b.width;
    g_mem_h = b.height;
  }
  // Converted now (guest memory as of the flip), copied into the texture in stream order.
  std::vector<u8> px(size_t(b.width) * b.height * 4);
  for (u32 y = 0; y < b.height; ++y) {
    const u8* src = ::ppu::mem() + ea + y * b.pitch;
    u8* dst = px.data() + size_t(y) * b.width * 4;
    for (u32 x = 0; x < b.width; ++x) {  // guest A8R8G8B8 (bytes A,R,G,B) -> B,G,R,A
      dst[4 * x + 0] = src[4 * x + 3];
      dst[4 * x + 1] = src[4 * x + 2];
      dst[4 * x + 2] = src[4 * x + 1];
      dst[4 * x + 3] = 0xff;
    }
  }
  gpu::rec().call([tex = g_mem_tex, srv = g_mem_srv, w = b.width, h = b.height, px = std::move(px)](ID3D11DeviceContext* c) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(c->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      for (u32 y = 0; y < h; ++y) std::memcpy(static_cast<u8*>(ms.pData) + size_t(y) * ms.RowPitch, px.data() + size_t(y) * w * 4, size_t(w) * 4);
      c->Unmap(tex, 0);
    }
    gpu::present_texture(srv, w, h);
  });
}

}  // namespace

int phase_switch(int p) {
  u64 now = __rdtsc();  // a QueryPerformanceCounter call per phase change was ~3% of the FIFO thread
  if (g_phase_at) g_phase_ticks[g_phase] += now - g_phase_at;
  g_phase_at = now;
  int prev = g_phase;
  g_phase = p;
  return prev;
}

// ---------------------------------------------------------------------------------------------
// Pixel counts (ZPASS_PIXEL_CNT reports, rsx_facts §8): the samples passing the depth/stencil
// tests while counting is enabled (SET_ZPASS_PIXEL_COUNT_ENABLE) since the last
// CLEAR_REPORT_VALUE(1). Each counting span is a D3D11 occlusion query. A report gets at once the
// last count known for its address (AC4 asks about every other frame in missions and reads the
// value later), and the submission thread writes the real count into guest memory once the GPU
// has it (polled at frame ends). Was always 0 before (nothing ever counted as visible). Counts are
// in the game's samples: at a render scale a sample covers more storage pixels (the sun glare's
// strength follows the count).
namespace {
struct ZpSpan {
  ID3D11Query* q;
  float area;  // storage pixels per game sample while it counted
};
ID3D11Query* g_zp_open = nullptr;        // FIFO thread: the query of the counting span in progress
std::vector<ZpSpan> g_zp_closed;         // FIFO thread: spans ended since the last clear
bool g_zp_used = false;                  // FIFO thread: a pixel count was reported (frame-end polls)
bool g_zp_on = true;                     // "gpu zpass on|off" (A/B: off answers 0 as before)
std::mutex g_zp_mu;
std::unordered_map<u32, u32> g_zp_last;  // report address -> last count written (under g_zp_mu)
struct ZpReport {
  u32 ea;
  std::vector<ZpSpan> qs;
};
std::vector<ZpReport> g_zp_pending;  // submission thread (recorded calls) only

void zpass_begin() {
  D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
  ID3D11Query* q = nullptr;
  if (FAILED(device().dev->CreateQuery(&qd, &q))) return;
  gpu::rec().call([q](ID3D11DeviceContext* c) { c->Begin(q); });
  g_zp_open = q;
}
void zpass_end() {
  ID3D11Query* q = g_zp_open;
  if (!q) return;
  gpu::rec().call([q](ID3D11DeviceContext* c) { c->End(q); });
  g_zp_closed.push_back({q, std::max(g_last_area, 1e-3f)});
  g_zp_open = nullptr;
  if (g_zp_closed.size() == 1024) warn_once("zpass-spans", "pixel counting: 1024 spans without CLEAR_REPORT_VALUE");
}
void zpass_enable(bool on) {
  if (fr_on()) LOG_I("frame", "pixel counting %s", on ? "on" : "off");
  if (!g_have_device || (on && !g_zp_on)) return;
  if (on && !g_zp_open) zpass_begin();
  else if (!on) zpass_end();
}
void zpass_clear() {
  bool counting = g_zp_open != nullptr;
  zpass_end();
  for (const ZpSpan& z : g_zp_closed) gpu::rec().release(z.q);
  g_zp_closed.clear();
  if (counting) zpass_begin();  // counting stays enabled across the clear
}
// Submission thread: writes the counts of the reports whose queries have finished.
void zpass_poll(ID3D11DeviceContext* c) {
  for (auto it = g_zp_pending.begin(); it != g_zp_pending.end();) {
    double total = 0;
    bool done = true;
    for (const ZpSpan& z : it->qs) {
      UINT64 n = 0;
      if (c->GetData(z.q, &n, sizeof n, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        done = false;
        break;
      }
      total += double(n) / z.area;
    }
    if (!done) {
      ++it;
      continue;
    }
    u32 v = u32(std::min(std::round(total), 4294967295.0));
    static int logged = 0;
    if (logged < 6) {
      ++logged;
      LOG_I("gpu", "pixel count report at 0x%08x: %u samples (%zu spans)", it->ea, v, it->qs.size());
    }
    ppu::w32(it->ea + 8, v);
    {
      std::lock_guard lk(g_zp_mu);
      g_zp_last[it->ea] = v;
    }
    for (const ZpSpan& z : it->qs) z.q->Release();
    it = g_zp_pending.erase(it);
  }
}
u32 zpass_report(u32 ea) {
  if (fr_on()) LOG_I("frame", "pixel count report to 0x%08x", ea);
  if (!g_have_device || !g_zp_on) return 0;
  g_zp_used = true;
  if (g_zp_open) {  // the count so far: this span ends, counting goes on in a new one
    zpass_end();
    zpass_begin();
  }
  std::vector<ZpSpan> qs = g_zp_closed;
  for (const ZpSpan& z : qs) z.q->AddRef();  // the resolver's references
  gpu::rec().call([qs, ea](ID3D11DeviceContext*) { g_zp_pending.push_back({ea, qs}); });
  std::lock_guard lk(g_zp_mu);
  auto it = g_zp_last.find(ea);
  return it == g_zp_last.end() ? 0 : it->second;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Hooks
void renderer_thread_init() {
  void* hwnd = nullptr;
  if (!cfg().headless) {
    for (int i = 0; i < 500 && !(hwnd = gpu::window_handle()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  g_have_device = gpu::device_init(hwnd);
  if (!g_have_device) {
    LOG_E("gpu", "no D3D11 device: rendering disabled");
    return;
  }
  gpu::submit_init(cfg().gpu_submit_thread);
  gpu::g_texture_bc = cfg().texture_bc;
  auto* dev = device().dev;
  ID3DBlob* vs = gpu::compile_hlsl(kClearHlsl, "vs_main", "vs_5_0");
  ID3DBlob* ps = gpu::compile_hlsl(kClearHlsl, "ps_main", "ps_5_0");
  if (vs) dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_clear_vs);
  if (ps) dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_clear_ps);
  release(vs);
  release(ps);
  auto cb = [&](u32 size, ID3D11Buffer** b) {
    D3D11_BUFFER_DESC bd{(size + 15) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&bd, nullptr, b);
  };
  cb(32, &g_clear_cb);
  cb(sizeof g_vc, &g_vc_buf);
  cb(sizeof(VsState), &g_vs_state);
  cb(sizeof(gpu::VtxFmt), &g_vtx_fmt);
  cb(sizeof(FpState), &g_fp_state);
  cb(256 * 16, &g_fp_const);
  shadow_reset();
  for (auto& v : g_imm) {
    v[0] = v[1] = v[2] = 0;
    v[3] = 1;
  }
}

void renderer_idle() {}

// Frame pacing: intervals between presented frames, summarized every 5 s ("frame times" lines);
// single frames of 100 ms or more are logged as stalls with their time, and the window reports
// how many shaders were compiled in it (first use) so warmed-up results can be told apart.
void pacing_frame() {
  static double window_start = -1, last = -1;
  static std::vector<float> ms;
  static u32 compiled_at_start = 0;
  static int stalls_logged = 0;
  static RenderStats prev;
  static u64 prev_idle = 0;
  static u64 prev_ticks[kPhCount] = {}, window_ticks[kPhCount] = {};
  static u64 window_idle = 0;
  // Phase ticks are time stamp counter ticks (phase_switch); their rate is measured against the
  // performance counter since the first frame.
  static u64 tsc0 = 0;
  static LARGE_INTEGER qpc0{}, qpf{};
  double& ms_per_tick = g_ms_per_tick;
  ms_per_tick = 0;
  {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    u64 tsc = __rdtsc();
    if (!tsc0) {
      tsc0 = tsc;
      qpc0 = q;
      QueryPerformanceFrequency(&qpf);
    } else if (tsc > tsc0) {
      ms_per_tick = double(q.QuadPart - qpc0.QuadPart) * 1000.0 / double(qpf.QuadPart) / double(tsc - tsc0);
    }
  }
  phase_switch(g_phase);  // account the current phase up to now
  double now = log_time();
  u32 compiled = 0, cached = 0;
  double secs = 0;
  u64 idle = g_fifo_idle_us.load(std::memory_order_relaxed);
  if (last >= 0) {
    float dt = float((now - last) * 1000);
    ms.push_back(dt);
    if (dt >= 100 && stalls_logged < 200) {  // what this frame did, and how long the FIFO waited for the game
      ++stalls_logged;
      gpu::shader_stats(&compiled, &cached, &secs);
      LOG_I("perf",
            "stall: %.0f ms frame ending at %.2f s: FIFO idle %.0f ms, %llu draws, %llu textures uploaded, %llu surfaces created, "
            "%llu vertex conversions of %llu vertices (largest draw %llu) (%u shaders compiled so far)",
            dt, now, (idle - prev_idle) / 1000.0, (unsigned long long)(g_rs.draws - prev.draws),
            (unsigned long long)(g_rs.tex_uploaded - prev.tex_uploaded), (unsigned long long)(g_rs.surfaces - prev.surfaces),
            (unsigned long long)(g_rs.vconverted - prev.vconverted), (unsigned long long)(g_rs.vertices - prev.vertices),
            (unsigned long long)g_rs.max_draw_vertices, compiled);
      std::string times;
      for (int p = 0; p < kPhCount; ++p) {
        double t = double(g_phase_ticks[p] - prev_ticks[p]) * ms_per_tick;
        if (p == kPhMethods) t -= (idle - prev_idle) / 1000.0;
        if (t >= 0.5) times += ac4::strprintf("%s%s %.0f", times.empty() ? "" : ", ", kPhaseNames[p], t);
      }
      LOG_I("perf", "  FIFO thread time (ms): %s", times.empty() ? "-" : times.c_str());
    }
  }
  std::memcpy(prev_ticks, g_phase_ticks, sizeof prev_ticks);
  g_rs.max_draw_vertices = 0;  // per frame
  prev = g_rs;
  prev_idle = idle;
  last = now;
  if (window_start < 0) {
    window_start = now;
    gpu::shader_stats(&compiled_at_start, &cached, &secs);
    std::memcpy(window_ticks, g_phase_ticks, sizeof window_ticks);
    window_idle = idle;
  }
  if (now - window_start < 5.0 || ms.empty()) return;
  std::vector<float> v = ms;
  std::sort(v.begin(), v.end());
  auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(p * double(v.size())))]; };
  double total = 0;
  u32 over33 = 0, over50 = 0, over100 = 0;
  for (float x : v) {
    total += x;
    over33 += x > 33.4f;
    over50 += x > 50.0f;
    over100 += x >= 100.0f;
  }
  gpu::shader_stats(&compiled, &cached, &secs);
  LOG_I("perf", "frame times %.1f s: %zu frames, mean %.1f ms (%.1f fps), median %.1f, p90 %.1f, p99 %.1f, max %.1f ms; "
        ">33 ms %u, >50 ms %u, >=100 ms %u; %u shaders compiled",
        now - window_start, v.size(), total / double(v.size()), 1000.0 * double(v.size()) / total, pct(0.5), pct(0.9), pct(0.99),
        v.back(), over33, over50, over100, compiled - compiled_at_start);
  {  // FIFO thread time per frame by phase (FIFO idle reported separately)
    std::string times;
    double n = double(v.size());
    for (int p = 0; p < kPhCount; ++p) {
      double t = double(g_phase_ticks[p] - window_ticks[p]) * ms_per_tick;
      if (p == kPhMethods) t -= (idle - window_idle) / 1000.0;
      if (t / n >= 0.05) times += ac4::strprintf(", %s %.1f", kPhaseNames[p], t / n);
    }
    // The thread's own CPU time (cycles it ran, counted by the OS; TSC rate measured over the
    // window): unlike the phase times it excludes time the thread waited for a CPU, so it stays
    // comparable when other programs load the machine.
    static u64 window_cycles = 0, window_tsc = 0;
    static double window_wall = 0;
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    u64 tsc = __rdtsc();
    std::string cpu;
    if (window_tsc && now > window_wall) {
      double tsc_per_ms = double(tsc - window_tsc) / ((now - window_wall) * 1000.0);
      cpu = ac4::strprintf("; on CPU %.1f", double(cycles - window_cycles) / tsc_per_ms / n);
    }
    // The submission thread (submit.h): time executing D3D11 calls and its own CPU time.
    static double sub_busy_prev = 0;
    static u64 sub_cycles_prev = 0;
    double sub_busy = 0;
    u64 sub_cycles = 0;
    gpu::submit_stats(&sub_busy, &sub_cycles);
    if (window_tsc && now > window_wall && gpu::rec().async()) {
      double tsc_per_ms = double(tsc - window_tsc) / ((now - window_wall) * 1000.0);
      cpu += ac4::strprintf("; submit thread: busy %.1f, on CPU %.1f (%s)", (sub_busy - sub_busy_prev) / n,
                             double(sub_cycles - sub_cycles_prev) / tsc_per_ms / n, gpu::submit_memory().c_str());
    }
    sub_busy_prev = sub_busy;
    sub_cycles_prev = sub_cycles;
    window_cycles = cycles;
    window_tsc = tsc;
    window_wall = now;
    LOG_I("perf", "  FIFO thread ms per frame: idle %.1f%s%s", (idle - window_idle) / 1000.0 / n, times.c_str(), cpu.c_str());
    std::memcpy(window_ticks, g_phase_ticks, sizeof window_ticks);
    window_idle = idle;
  }
  ms.clear();
  window_start = now;
  compiled_at_start = compiled;
}

void present_buffer(u32 id, const DisplayBuffer& b) {
  if (!g_have_device || !b.width || !b.height) return;
  if (fr_on()) LOG_I("frame", "flip buffer %u at 0x%x (%ux%u)", id, b.offset, b.width, b.height);
  pacing_frame();
  PhaseScope phase_scope(kPhFlip);  // counted in the next frame, which it delays
  fr_frame_boundary();
  vdata_trim();
  tex_trim();
  surface_trim();
  g_writes_stale = true;
  if (g_frame && g_frame % 300 == 0) {
    auto pf = [](u64 RenderStats::*f) { return double(g_rs.*f - g_rs_last.*f) / 300.0; };
    u64 surf_bytes = 0;
    for (const Surface* s : g_surfaces) surf_bytes += surface_bytes(s);
    LOG_I("perf",
          "per frame: %.0f draws (%.1f conditional, %.1f of them skipped; %.1f pixel count and %.1f zcull reports; vertex data %.0f cached, %.0f uploaded: %.0f KiB, element span %.0f), textures %.1f hashed "
          "%.1f uploaded, %.2f surfaces created, %.1f target changes, %.1f collections (%.0f write queries over %.0f MiB, "
          "%.2f ms, %.0f pages written); static vertex data %zu entries %.0f MiB; textures %zu %.0f MiB; surfaces %zu %.0f MiB",
          pf(&RenderStats::draws), pf(&RenderStats::cond_draws), pf(&RenderStats::cond_skipped), pf(&RenderStats::zpass_reports),
          pf(&RenderStats::zcull_reports), pf(&RenderStats::vcached),
          pf(&RenderStats::vconverted), pf(&RenderStats::vbytes) / 1024.0,
          pf(&RenderStats::vertices), pf(&RenderStats::tex_hashed), pf(&RenderStats::tex_uploaded), pf(&RenderStats::surfaces),
          pf(&RenderStats::target_binds), pf(&RenderStats::collections), pf(&RenderStats::query_calls),
          pf(&RenderStats::bytes_queried) / 1048576.0, pf(&RenderStats::query_ticks) * g_ms_per_tick, pf(&RenderStats::query_written),
          g_streams.size() + g_index_lists.size(), g_vdata_bytes / 1048576.0, g_tex.size(), g_tex_bytes / 1048576.0,
          g_surfaces.size(), surf_bytes / 1048576.0);
    g_rs_last = g_rs;
  }
  ++g_frame;
  gpu::timing::mark("present");
  update_render_scale();
  Surface* s = find_surface(0, b.offset);
  if (s && !s->depth) {
    ID3D11RenderTargetView* none = nullptr;
    gpu::rec().OMSetRenderTargets(1, &none, nullptr);
    gpu::rec().call([srv = s->srv, w = s->width, h = s->height](ID3D11DeviceContext*) { gpu::present_texture(srv, w, h); });
  } else {
    present_memory(b);
  }
  shadow_reset();
  maybe_capture();
  gpu::timing::frame_end();
  g_rt_label = "-";
  if (g_zp_used) gpu::rec().call([](ID3D11DeviceContext* c) { zpass_poll(c); });  // pixel count results
  gpu::rec().frame_end(1);  // at most one presented frame queued behind the FIFO thread
  gpu::apply_submit_mode();
}

void renderer_mark_special(void (*mark)(u32 method, u32 bytes)) {
  mark(m::SET_TRANSFORM_PROGRAM, 0x80);
  mark(m::SET_TRANSFORM_CONSTANT, 0x80);
  mark(m::SET_VERTEX_DATA4F_M, 0x100);
  mark(m::SET_VERTEX_DATA2F_M, 0x80);
  mark(m::SET_VERTEX_DATA1F_M, 0x40);
  mark(m::SET_VERTEX_DATA4UB_M, 0x40);
  mark(m::SET_VERTEX_DATA3F_M, 0x100);
  for (u32 method : {m::SET_TRANSFORM_PROGRAM_LOAD, m::SET_TRANSFORM_CONSTANT_LOAD, m::SET_BEGIN_END, m::DRAW_ARRAYS,
                     m::DRAW_INDEX_ARRAY, m::INLINE_ARRAY, m::ARRAY_ELEMENT16, m::ARRAY_ELEMENT32, m::CLEAR_SURFACE,
                     m::SET_ZPASS_PIXEL_COUNT_ENABLE, m::CLEAR_REPORT_VALUE})
    mark(method, 4);
}

void renderer_transform_constants(const u8* words, u32 first, u32 count) {
  u32 base = g_vc_load * 4 + first;
  if (base >= 468 * 4) return;
  count = std::min(count, 468 * 4 - base);
  for (u32 i = 0; i < count; ++i) {
    u32 w, old;
    std::memcpy(&w, words + 4 * i, 4);
    w = __builtin_bswap32(w);
    std::memcpy(&old, &g_vc[base + i], 4);
    if (old == w) continue;
    std::memcpy(&g_vc[base + i], &w, 4);
    g_vc_dirty = true;
    g_vc_lo = std::min(g_vc_lo, (base + i) >> 2);
    g_vc_hi = std::max(g_vc_hi, (base + i) >> 2);
  }
}

void renderer_method(u32 ri, u32 arg, u32 count, u32 index) {
  u32 method = ri << 2;
  if (method >= m::SET_TRANSFORM_PROGRAM && method < m::SET_TRANSFORM_PROGRAM + 0x80) {
    if (index == 0) g_vp_burst = g_vp_load * 4 + ((method - m::SET_TRANSFORM_PROGRAM) >> 2) % 4;
    u32 word = g_vp_burst + index;
    if (word < gpu::kVpSlots * 4 && g_vp[word] != arg) {
      g_vp[word] = arg;
      ++g_vp_gen;
    }
    if (index + 1 == count) g_vp_load += count / 4;
    return;
  }
  if (method >= m::SET_TRANSFORM_CONSTANT && method < m::SET_TRANSFORM_CONSTANT + 0x80) {
    u32 word = g_vc_load * 4 + ((method - m::SET_TRANSFORM_CONSTANT) >> 2);
    u32 old;
    if (word < 468 * 4 && (std::memcpy(&old, &g_vc[word], 4), old != arg)) {
      std::memcpy(&g_vc[word], &arg, 4);
      g_vc_dirty = true;
      g_vc_lo = std::min(g_vc_lo, word >> 2);
      g_vc_hi = std::max(g_vc_hi, word >> 2);
    }
    return;
  }
  // Immediate vertex data (rsx_facts.md §4.4).
  if (method >= m::SET_VERTEX_DATA4F_M && method < m::SET_VERTEX_DATA4F_M + 0x100) {
    u32 a = (method - m::SET_VERTEX_DATA4F_M) >> 4, c = ((method - m::SET_VERTEX_DATA4F_M) >> 2) & 3;
    float v[4];
    v[c] = f32_of(arg);
    if (c == 3 || a != 0) set_immediate(a, 1u << c, v);
    else g_imm[a][c] = v[c];
    return;
  }
  if (method >= m::SET_VERTEX_DATA2F_M && method < m::SET_VERTEX_DATA2F_M + 0x80) {
    u32 a = (method - m::SET_VERTEX_DATA2F_M) >> 3, c = ((method - m::SET_VERTEX_DATA2F_M) >> 2) & 1;
    float v[4] = {0, 0, 0, 1};
    v[c] = f32_of(arg);
    if (c == 1) {
      g_imm[a][2] = 0;
      g_imm[a][3] = 1;
      set_immediate(a, 2, v);
    } else {
      g_imm[a][0] = v[0];
    }
    return;
  }
  if (method >= m::SET_VERTEX_DATA1F_M && method < m::SET_VERTEX_DATA1F_M + 0x40) {
    u32 a = (method - m::SET_VERTEX_DATA1F_M) >> 2;
    float v[4] = {f32_of(arg), 0, 0, 1};
    set_immediate(a, 0xf, v);
    return;
  }
  if (method >= m::SET_VERTEX_DATA4UB_M && method < m::SET_VERTEX_DATA4UB_M + 0x40) {
    u32 a = (method - m::SET_VERTEX_DATA4UB_M) >> 2;
    float v[4] = {((arg >> 24) & 0xff) / 255.0f, ((arg >> 16) & 0xff) / 255.0f, ((arg >> 8) & 0xff) / 255.0f, (arg & 0xff) / 255.0f};
    set_immediate(a, 0xf, v);
    return;
  }
  if (method >= m::SET_VERTEX_DATA3F_M && method < m::SET_VERTEX_DATA3F_M + 0x100) {
    u32 a = (method - m::SET_VERTEX_DATA3F_M) >> 4, c = ((method - m::SET_VERTEX_DATA3F_M) >> 2) & 3;
    if (c < 3) {
      g_imm[a][c] = f32_of(arg);
      if (c == 2) {
        g_imm[a][3] = 1;
        set_immediate(a, 0, g_imm[a]);
      }
    }
    return;
  }
  switch (method) {
    case m::SET_TRANSFORM_PROGRAM_LOAD:
      g_vp_load = arg;
      return;
    case m::SET_TRANSFORM_CONSTANT_LOAD:
      g_vc_load = arg;
      return;
    case m::SET_BEGIN_END:
      if (arg & 0xff) {
        reset_clause();
        g_prim = arg & 0xff;
      } else {
        flush_draw();
        reset_clause();
        g_prim = 0;
      }
      return;
    case m::DRAW_ARRAYS:
      if (g_clause != Clause::Arrays) {
        g_ranges.clear();
        g_clause = Clause::Arrays;
      }
      g_ranges.push_back({arg & 0xffffff, (arg >> 24) + 1});
      return;
    case m::DRAW_INDEX_ARRAY:
      if (g_clause != Clause::Indexed) {
        g_ranges.clear();
        g_clause = Clause::Indexed;
      }
      g_ranges.push_back({arg & 0xffffff, (arg >> 24) + 1});
      return;
    case m::INLINE_ARRAY:
      g_clause = Clause::Inline;
      g_inline.push_back(arg);
      return;
    case m::ARRAY_ELEMENT16:
      g_clause = Clause::Elements;
      g_elements.push_back(arg & 0xffff);
      g_elements.push_back(arg >> 16);
      return;
    case m::ARRAY_ELEMENT32:
      g_clause = Clause::Elements;
      g_elements.push_back(arg);
      return;
    case m::CLEAR_SURFACE:
      if (g_have_device) clear_surface(arg);
      return;
    case m::SET_ZPASS_PIXEL_COUNT_ENABLE:
      zpass_enable(arg != 0);
      return;
    case m::CLEAR_REPORT_VALUE:
      if (arg == 1) zpass_clear();
      return;
    default:
      return;
  }
}

u32 g_covered_put = ~0u;

void renderer_guest_sync() { g_writes_stale = true; }

void renderer_memory_written(u32 ea, u32 size) {
  // The RSX wrote guest memory: data cached from those pages is stale from now on, before a
  // collection would report it.
  if (g_page_serial.empty() || !size) return;
  ++g_mem_serial;
  for (u32 p = ea >> 12, e = u32(std::min<u64>((u64(ea) + size + 0xfff) >> 12, 1u << 20)); p < e; ++p) g_page_serial[p] = g_mem_serial;
}
void renderer_flush() {}

// Reports (rsx_facts §8): zcull statistics (types 2-4) are not modelled; as RPCS3 they read -1
// while zcull and its statistics are enabled, else 0 (AC4 requests type 2). Pixel counts (type
// 1) come from occlusion queries ("Pixel counts" above). ZCULL_STATS3 is answered with 0. "gpu
// zcullstats off" answers 0 for zcull statistics.
u32 renderer_report_value(u32 type, u32 ea) {
  if (type == 1) {
    ++g_rs.zpass_reports;
    return zpass_report(ea);
  }
  ++g_rs.zcull_reports;
  if (type >= 2 && type <= 4 && g_zcull_stats)
    return reg(m::SET_ZCULL_STATS_ENABLE) && reg(m::SET_ZCULL_EN) ? 0xffffffffu : 0u;
  warn_once("report" + std::to_string(type), "GET_REPORT type " + std::to_string(type) + " answered with 0 (not modelled)");
  return 0;
}

}  // namespace rt::rsx
