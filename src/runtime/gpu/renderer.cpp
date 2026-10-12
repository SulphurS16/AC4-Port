// RSX renderer (stage 2): renders NV4097 draws with Direct3D 11 (rsx_facts.md §1-§8).
//
// Surfaces live on the GPU, keyed by (location, offset); a flip presents the color surface at the
// display buffer's offset, or guest memory when nothing was rendered there. Vertex programs and
// fragment programs are translated to HLSL (vp_hlsl/fp_hlsl) and cached. Vertex data is fetched
// and converted to float4 on the CPU. Known gaps are logged once and listed in docs/STATUS.md.
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>
#include <unordered_map>

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
      bytes_queried = 0, vertices = 0, max_draw_vertices = 0;
};
RenderStats g_rs, g_rs_last;

// Phase timing (rsx_internal.h): performance counter ticks per phase.
const char* const kPhaseNames[kPhCount] = {"methods",    "surfaces", "shaders", "textures", "vertices",
                                           "write checks", "draw calls", "flip", "waits", "state",
                                           "constants",  "uploads"};
u64 g_phase_ticks[kPhCount];
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
  ID3D11ShaderResourceView* vs_srv;
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
// Sets the slots of want[16] that differ from have[16] (one call over the differing span).
template <typename T, typename F>
void bind_changed(T** want, T** have, F set) {
  u32 lo = 16, hi = 0;
  for (u32 i = 0; i < 16; ++i)
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
  u32 location = 0, offset = 0, format = 0, width = 0, height = 0;
  bool depth = false;
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
u64 g_write_serial = 0;

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
  for (Surface* s : g_surfaces)
    if (s->location == location && s->offset == offset) return s;
  return nullptr;
}

u64 surface_bytes(const Surface* s) {
  u32 bpp = s->depth ? (s->format == 1 ? 2 : 4) : s->format == 11 ? 8 : s->format == 12 ? 16 : 4;
  return u64(s->width) * s->height * bpp * (s->copy ? 2 : 1);
}
void shadow_reset();
// Render targets of earlier scenes stay until their address is reused: once they exceed the
// budget, surfaces neither rendered to nor sampled for a minute are destroyed.
void surface_trim() {
  constexpr u64 kBudget = 512ull << 20;
  u64 total = 0;
  for (const Surface* s : g_surfaces) total += surface_bytes(s);
  if (total <= kBudget) return;
  u32 dropped = 0;
  for (auto it = g_surfaces.begin(); it != g_surfaces.end();) {
    Surface* s = *it;
    if (s->last_frame + 3600 < g_frame) {
      total -= surface_bytes(s);
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

Surface* get_surface(u32 location, u32 offset, u32 format, u32 w, u32 h, bool depth) {
  Surface* s = find_surface(location, offset);
  if (s && s->depth == depth && s->format == format && s->width == w && s->height == h) {
    s->last_frame = g_frame;
    return s;
  }
  if (s) {
    static int logged = 0;
    if (logged < 24) {
      ++logged;
      LOG_I("gpu", "surface at %s 0x%08x recreated: %s %ux%u fmt %u -> %s %ux%u fmt %u", location ? "main" : "local", offset,
            s->depth ? "depth" : "color", s->width, s->height, s->format, depth ? "depth" : "color", w, h, format);
    }
    g_surfaces.erase(std::find(g_surfaces.begin(), g_surfaces.end(), s));
    destroy(s);
  }
  s = new Surface{location, offset, format, w, h, depth};
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
  u32 sx = 1, sy = 1;
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
  const u32 sw = t.width * t.sx, shh = t.height * t.sy;  // storage size
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
    t.color[t.count++] = get_surface(dma_location(reg(kDmaReg[idx[i]])), reg(kOffsetReg[idx[i]]), t.color_format, sw, shh, false);
  if (want_depth && t.depth_format)
    t.depth = get_surface(dma_location(reg(m::SET_CONTEXT_DMA_ZETA)), reg(m::SET_SURFACE_ZETA_OFFSET), t.depth_format, sw, shh, true);
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
  r.left *= LONG(t.sx), r.right *= LONG(t.sx), r.top *= LONG(t.sy), r.bottom *= LONG(t.sy);  // storage pixels
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
  bool full = r.left == 0 && r.top == 0 && r.right == LONG(t.width * t.sx) && r.bottom == LONG(t.height * t.sy);
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
bool g_vc_dirty = true;
// Words of g_vc written since the last upload, [lo, hi): only those constants are recorded (heavy
// scenes change one or two of the 512 constants per draw; the submission side uploads whole).
u32 g_vc_lo = 0, g_vc_hi = 512 * 4;
inline void vc_mark(u32 word, u32 n) {
  g_vc_dirty = true;
  g_vc_lo = std::min(g_vc_lo, word);
  g_vc_hi = std::max(g_vc_hi, word + n);
}

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
  u32 w = 0, h = 0, d = 1;
  u64 bytes = 0, last_used = 0;  // host texture size; frame of the last lookup (cache trimming)
  // Contents changed after the first upload (movie frames, render-time data): the host texture
  // is updated in place (DEFAULT usage) and a written texture is converted without hashing.
  bool dynamic = false;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  u32 mips = 0, faces = 0;
};

// Guest page write tracking: per 4 KiB page, the serial of the last collection that saw it
// written (or of an RSX transfer that wrote it, renderer_memory_written).
std::vector<u32> g_page_serial;
u32 g_mem_serial = 1;
bool g_track_writes = true;
// Only the 64 KiB granules holding cached data are queried (a query costs in proportion to the
// range). A collection queries the granules a cache check used in the last kHotFrames frames, in
// runs; the others keep their written state in the OS (it accumulates until reset) and are queried
// when a check next touches them (refresh_granules), so data idle for a while costs nothing.
constexpr u32 kGranuleShift = 16;
constexpr u32 kGranules = 1u << (32 - kGranuleShift);
constexpr u64 kHotFrames = 2;
std::vector<u8> g_granule_watched(kGranules);
std::vector<u32> g_granule_queried(kGranules);  // collection epoch of the last query
std::vector<u64> g_granule_used(kGranules);     // frame of the last cache check touching it
u32 g_collect_epoch = 0;
void watch_range(u32 ea, u32 size) {
  for (u32 r = ea >> kGranuleShift, e = u32((u64(ea) + size + (1u << kGranuleShift) - 1) >> kGranuleShift); r < e && r < kGranules;
       ++r)
    g_granule_watched[r] = 1;
}
bool query_pages(u32 first, u32 end, std::vector<u32>& pages) {
  g_rs.bytes_queried += u64(end - first) << kGranuleShift;
  if (vm::take_written_pages(first << kGranuleShift, (end - first) << kGranuleShift, pages)) return true;
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
    if (!g_granule_watched[r] || g_granule_queried[r] == g_collect_epoch) continue;
    u32 run_end = r + 1;
    while (run_end < e && g_granule_watched[run_end] && g_granule_queried[run_end] != g_collect_epoch) g_granule_used[run_end++] = g_frame;
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
  ++g_collect_epoch;  // every granule is unqueried in the new epoch
  if (g_page_serial.empty()) g_page_serial.assign(1u << 20, 0);
  // Hot granules in runs; small gaps are queried too (fewer calls for about the same cost).
  static std::vector<std::pair<u32, u32>> runs;
  runs.clear();
  for (u32 r = 0; r < kGranules; ++r) {
    if (!g_granule_watched[r] || g_granule_used[r] + kHotFrames < g_frame) continue;
    if (!runs.empty() && r - runs.back().second <= 2) runs.back().second = r + 1;
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
  // The pass runs while a draw is being prepared: everything it changes is saved and restored.
  // It runs as one closure in stream order (submit.h), where the state it saves is current.
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_DEPTH_STENCIL_DESC dd{};
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  ID3D11BlendState* pass_bs = blend_state(bd);
  ID3D11DepthStencilState* pass_ds = depth_state(dd);
  ID3D11RasterizerState* pass_rs = raster_state(rd);
  ID3D11RenderTargetView* out_rtv = s->as_color_rtv;
  ID3D11ShaderResourceView* in[2] = {s->srv, s->stencil_srv};
  u32 width = s->width, height = s->height;
  gpu::timing::mark(ac4::strprintf("depth as color %ux%u", width, height));
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
  ID3D11ShaderResourceView* none[2] = {};
  c->OMSetRenderTargets(1, &out_rtv, nullptr);
  D3D11_VIEWPORT v{0, 0, float(width), float(height), 0, 1};
  c->RSSetViewports(1, &v);
  c->OMSetBlendState(pass_bs, nullptr, 0xffffffff);
  c->OMSetDepthStencilState(pass_ds, 0);
  c->RSSetState(pass_rs);
  c->IASetInputLayout(nullptr);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(g_d2c_vs, nullptr, 0);
  c->PSSetShader(g_d2c_ps, nullptr, 0);
  c->PSSetShaderResources(0, 2, in);
  c->Draw(3, 0);
  c->PSSetShaderResources(0, 2, none);
  c->OMSetRenderTargets(4, rtvs, dsv);
  for (auto* r : rtvs)
    if (r) r->Release();  // references taken by the Get calls above (this thread)
  if (dsv) dsv->Release();
  if (nvp) c->RSSetViewports(nvp, vp);
  c->OMSetBlendState(old_bs, old_bf, old_mask);
  c->OMSetDepthStencilState(old_ds, old_ref);
  c->RSSetState(old_rs);
  if (nsc) c->RSSetScissorRects(nsc, old_sc);
  c->IASetPrimitiveTopology(old_topo);
  c->VSSetShader(old_vs, nullptr, 0);
  c->PSSetShader(old_ps, nullptr, 0);
  for (IUnknown* o : {static_cast<IUnknown*>(old_bs), static_cast<IUnknown*>(old_ds), static_cast<IUnknown*>(old_rs),
                      static_cast<IUnknown*>(old_vs), static_cast<IUnknown*>(old_ps)})
    if (o) o->Release();
  });
  gpu::timing::mark(g_rt_label);
  s->as_color_serial = s->written;
  shadow_reset();
  return s->as_color_srv;
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
    bool ok = (depth_fmt || t.format == 0x85 || t.format == 0x9e) && t.width <= s->width && t.height <= s->height;
    return ok ? s : nullptr;
  }
  if (depth_fmt) return nullptr;
  if (compressed) {
    if (t.width != s->width || t.height != s->height) return nullptr;
    warn_once("dxt-alias", "compressed textures at rendered surfaces read the rendered pixels (GPU results are not written back)");
    return s;
  }
  return t.width <= s->width && t.height <= s->height ? s : nullptr;
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

// Converts one element of `type` with `size` components at `p` to float4 (rsx_facts.md §4.2).
void convert_attr(u32 type, u32 size, const u8* p, float* out) {
  out[0] = out[1] = out[2] = 0;
  out[3] = 1;
  auto be16 = [](const u8* q) { return u16((q[0] << 8) | q[1]); };
  auto be32 = [](const u8* q) { return (u32(q[0]) << 24) | (u32(q[1]) << 16) | (u32(q[2]) << 8) | q[3]; };
  switch (type) {
    case 1:  // S16 normalized
      for (u32 c = 0; c < size; ++c) out[c] = (float(s16(be16(p + 2 * c))) + 0.5f) / 32767.5f;
      break;
    case 2:  // F32
      for (u32 c = 0; c < size; ++c) out[c] = f32_of(be32(p + 4 * c));
      break;
    case 3:  // F16
      for (u32 c = 0; c < size; ++c) out[c] = half_to_float(be16(p + 2 * c));
      break;
    case 4:  // U8 normalized
      for (u32 c = 0; c < size; ++c) out[c] = p[c] / 255.0f;
      break;
    case 5:  // S16 unnormalized
      for (u32 c = 0; c < size; ++c) out[c] = float(s16(be16(p + 2 * c)));
      break;
    case 6: {  // CMP 11:11:10
      u32 v = be32(p);
      out[0] = float(s16(u16((v & 0x7ff) << 5))) / 32767.0f;
      out[1] = float(s16(u16(((v >> 11) & 0x7ff) << 5))) / 32767.0f;
      out[2] = float(s16(u16(((v >> 22) & 0x3ff) << 6))) / 32767.0f;
      break;
    }
    case 0:
    case 7:  // U8 unnormalized
      for (u32 c = 0; c < size; ++c) out[c] = p[c];
      break;
    default:
      break;
  }
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

// Element converters specialized by type and component count (same results as convert_attr;
// the components beyond the count keep the template's 0, 0, 0, 1).
using ConvFn = void (*)(const u8*, float*);
template <u32 T, u32 N>
void conv(const u8* p, float* o) {
  for (u32 c = 0; c < N; ++c) {
    if constexpr (T == 2) {  // F32
      u32 w;
      std::memcpy(&w, p + 4 * c, 4);
      o[c] = f32_of(__builtin_bswap32(w));
    } else if constexpr (T == 1 || T == 3 || T == 5) {  // S16 normalized, F16, S16
      u16 h;
      std::memcpy(&h, p + 2 * c, 2);
      h = __builtin_bswap16(h);
      if constexpr (T == 1) o[c] = (float(s16(h)) + 0.5f) / 32767.5f;
      else if constexpr (T == 3) o[c] = half_to_float(h);
      else o[c] = float(s16(h));
    } else if constexpr (T == 4) {  // U8 normalized
      o[c] = p[c] / 255.0f;
    } else {  // U8 unnormalized (0, 7)
      o[c] = p[c];
    }
  }
}
void conv_cmp(const u8* p, float* o) { convert_attr(6, 1, p, o); }
void conv_none(const u8*, float*) {}
template <u32 T>
constexpr ConvFn conv_row[5] = {conv_none, conv<T, 1>, conv<T, 2>, conv<T, 3>, conv<T, 4>};
ConvFn conv_fn(u32 type, u32 size) {
  size = std::min(size, 4u);
  switch (type) {
    case 1: return conv_row<1>[size];
    case 2: return conv_row<2>[size];
    case 3: return conv_row<3>[size];
    case 4: return conv_row<4>[size];
    case 5: return conv_row<5>[size];
    case 6: return conv_cmp;
    case 0: return conv_row<0>[size];
    case 7: return conv_row<7>[size];
    default: return conv_none;
  }
}

// Vertices of a draw: 16 float4 attributes each, for the attributes the VP reads (unread ones
// 0, 0, 0, 1; registers for attributes without an array). The per-draw work (template vertex,
// active arrays with their converters) is done once; each vertex is the template plus its array
// elements. Also sets the element index range each array was read over (vertex cache).
u32 g_fetch_lo[16], g_fetch_hi[16];
void fetch_vertices(const AttrSrc* a, u32 input_mask, const std::vector<u32>& uniq, u32 lo, u32 hi, float* out) {
  struct Active {
    u32 slot;
    ConvFn fn;
    const u8* base;
    u32 stride, freq;
    bool modulo;
  };
  Active act[16];
  u32 nact = 0;
  alignas(16) float tmpl[64];
  for (u32 i = 0; i < 16; ++i) {
    float* t = tmpl + 4 * i;
    g_fetch_lo[i] = ~0u;
    g_fetch_hi[i] = 0;
    if (((input_mask >> i) & 1) && !a[i].array) {
      std::memcpy(t, g_imm[i], 16);
      continue;
    }
    t[0] = t[1] = t[2] = 0;
    t[3] = 1;
    if (!((input_mask >> i) & 1)) continue;
    act[nact++] = {i, conv_fn(a[i].type, a[i].size), ::ppu::mem() + a[i].ea, a[i].stride, a[i].freq, a[i].modulo};
    // Element index range over the draw's vertex indices [lo, hi].
    if (a[i].stride == 0) {
      g_fetch_lo[i] = g_fetch_hi[i] = 0;
    } else if (a[i].freq > 1 && a[i].modulo) {
      g_fetch_lo[i] = 0;
      g_fetch_hi[i] = std::min(hi, a[i].freq - 1);
    } else if (a[i].freq > 1) {
      g_fetch_lo[i] = lo / a[i].freq;
      g_fetch_hi[i] = hi / a[i].freq;
    } else {
      g_fetch_lo[i] = lo;
      g_fetch_hi[i] = hi;
    }
  }
  for (size_t v = 0; v < uniq.size(); ++v) {
    float* o = out + v * 64;
    std::memcpy(o, tmpl, sizeof tmpl);
    u32 k = uniq[v];
    for (u32 j = 0; j < nact; ++j) {
      const Active& t = act[j];
      u32 kk = k;
      if (t.freq > 1) kk = t.modulo ? k % t.freq : k / t.freq;
      if (t.stride == 0) kk = 0;
      t.fn(t.base + size_t(kk) * t.stride, o + 4 * t.slot);
    }
  }
}

ID3D11Buffer* g_vbuf = nullptr;
ID3D11ShaderResourceView* g_vsrv = nullptr;
u32 g_vbuf_cap = 0;
ID3D11Buffer* g_ibuf = nullptr;
u32 g_ibuf_cap = 0;

// Vertex and index data go into ring buffers: each draw appends (NO_OVERWRITE) and only a wrap
// discards, so the driver does not have to rename a large buffer for every draw.
u32 g_vbuf_pos = 0, g_ibuf_pos = 0;  // float4 / index units

// Appends the vertices (16 float4 each); returns the index of the first one in the buffer.
u32 upload_vertices(const std::vector<float>& v) {
  u32 count = u32(v.size() / 4);
  constexpr u32 kMinCap = 1u << 21;  // 2 M float4 = 32 MiB
  if (count > g_vbuf_cap || !g_vbuf) {
    release(g_vsrv);
    release(g_vbuf);
    g_vbuf_cap = std::max({count * 2, g_vbuf_cap * 2, kMinCap});
    D3D11_BUFFER_DESC bd{g_vbuf_cap * 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    device().dev->CreateBuffer(&bd, nullptr, &g_vbuf);
    device().dev->CreateShaderResourceView(g_vbuf, nullptr, &g_vsrv);
    g_vbuf_pos = g_vbuf_cap;  // force a discard on first use
  }
  D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (g_vbuf_pos + count > g_vbuf_cap) {
    mode = D3D11_MAP_WRITE_DISCARD;
    g_vbuf_pos = 0;
  }
  u32 base = g_vbuf_pos;
  gpu::rec().write_buffer(g_vbuf, mode, base * 16, v.data(), u32(v.size() * 4));
  g_vbuf_pos += count;
  return base / 16;  // a vertex is 16 float4
}
// Appends the indices; returns the start index for the draw.
u32 upload_indices(const std::vector<u32>& idx) {
  u32 n = u32(idx.size());
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
  gpu::rec().write_buffer(g_ibuf, mode, start * 4, idx.data(), u32(idx.size() * 4));
  g_ibuf_pos += n;
  return start;
}

// Converts the vertex index sequence of RSX primitive `prim` (with ~0u restart markers for strips)
// into a host topology and index list over positions in `seq`.
D3D11_PRIMITIVE_TOPOLOGY convert_primitive(u32 prim, const std::vector<u32>& seq, std::vector<u32>* out) {
  constexpr u32 R = ~0u;
  // Split at restart markers.
  std::vector<std::pair<u32, u32>> runs;
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

// ---------------------------------------------------------------------------------------------
// Diagnostics (--frame-report T): the first complete frame after T seconds is logged clear by
// clear and draw by draw (channel "frame"), and every surface it touched is written as PNG to
// <run>/frame/.
struct FrameReport {
  int state = 0;  // 0 waiting, 1 recording, 2 done
  u32 draws = 0, dropped = 0, clears = 0;
  std::vector<Surface*> touched;
  struct Tex {
    ID3D11ShaderResourceView* srv;  // referenced until written
    std::string name;
    u32 w, h;
  };
  std::vector<Tex> textures;  // memory textures sampled in the frame (2D)
} g_fr;
void fr_texture(ID3D11ShaderResourceView* srv, const gpu::TexDesc& t) {
  if (!srv || t.dim != 2 || t.cubemap || !t.width || !t.height) return;
  for (const auto& x : g_fr.textures)
    if (x.srv == srv) return;
  srv->AddRef();
  g_fr.textures.push_back({srv, ac4::strprintf("t_%s%08x_f%02x_%ux%u.png", t.location ? "m" : "l", t.offset, t.format, t.width, t.height),
                           t.width, t.height});
}
bool fr_on() { return g_fr.state == 1; }
void fr_forget(Surface* s) { g_fr.touched.erase(std::remove(g_fr.touched.begin(), g_fr.touched.end(), s), g_fr.touched.end()); }
void fr_touch(Surface* s) {
  if (s && std::find(g_fr.touched.begin(), g_fr.touched.end(), s) == g_fr.touched.end()) g_fr.touched.push_back(s);
}
std::string surf_desc(const Surface* s) {
  return s ? ac4::strprintf("%s%x %ux%u f%u", s->location ? "M:" : "", s->offset, s->width, s->height, s->format) : "-";
}
std::string targets_desc(const Targets& t) {
  std::string r = "rt[";
  for (u32 i = 0; i < t.count; ++i) r += (i ? ", " : "") + surf_desc(t.color[i]);
  return r + "] z[" + surf_desc(t.depth) + "]";
}
void fr_drop(const char* why) {
  if (!fr_on()) return;
  ++g_fr.dropped;
  LOG_I("frame", "  draw dropped: %s", why);
}

// Reads a surface back as 8-bit RGBA (float formats clamped, depth normalised to its range).
bool read_surface(Surface* s, std::vector<u8>& out, std::string* note) {
  gpu::rec().finish();  // the submission thread is idle until the next flush: direct context use
  D3D11_TEXTURE2D_DESC td{};
  s->tex->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(device().dev->CreateTexture2D(&td, nullptr, &st))) return false;
  device().ctx->CopyResource(st, s->tex);
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (FAILED(device().ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
    st->Release();
    return false;
  }
  u32 w = td.Width, h = td.Height;
  out.assign(size_t(w) * h * 4, 255);
  auto u8c = [](float v) { return u8(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  std::vector<float> depth;
  for (u32 y = 0; y < h; ++y) {
    const u8* row = static_cast<const u8*>(ms.pData) + size_t(y) * ms.RowPitch;
    u8* o = &out[size_t(y) * w * 4];
    for (u32 x = 0; x < w; ++x, o += 4) {
      switch (td.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: std::memcpy(o, row + 4 * x, 4); break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
          for (int c = 0; c < 4; ++c) {
            u16 hv;
            std::memcpy(&hv, row + 8 * x + 2 * c, 2);
            o[c] = u8c(half_to_float(hv));
          }
          break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
          for (int c = 0; c < 4; ++c) {
            float f;
            std::memcpy(&f, row + 16 * x + 4 * c, 4);
            o[c] = u8c(f);
          }
          break;
        case DXGI_FORMAT_R32_FLOAT: {
          float f;
          std::memcpy(&f, row + 4 * x, 4);
          o[0] = o[1] = o[2] = u8c(f);
          break;
        }
        case DXGI_FORMAT_R24G8_TYPELESS: {
          u32 v;
          std::memcpy(&v, row + 4 * x, 4);
          depth.push_back(float(v & 0xffffff) / 16777215.0f);
          break;
        }
        case DXGI_FORMAT_R16_TYPELESS: {
          u16 v;
          std::memcpy(&v, row + 2 * x, 2);
          depth.push_back(float(v) / 65535.0f);
          break;
        }
        default: break;
      }
    }
  }
  device().ctx->Unmap(st, 0);
  st->Release();
  if (!depth.empty()) {
    auto [lo, hi] = std::minmax_element(depth.begin(), depth.end());
    float a = *lo, b = *hi;
    *note = ac4::strprintf("depth range %.6f..%.6f", a, b);
    for (size_t i = 0; i < depth.size(); ++i) {
      u8 g = b > a ? u8c((depth[i] - a) / (b - a)) : 0;
      out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = g;
    }
  }
  return true;
}

// Writes mip 0 of a texture as PNG (decoded by the GPU: compressed formats included).
ID3D11VertexShader* g_tl_vs = nullptr;
ID3D11PixelShader* g_tl_ps = nullptr;
const char* kTexLoadHlsl = R"(
Texture2D t0 : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target0 { return t0.Load(int3(pos.xy, 0)); }
)";
bool dump_texture(ID3D11ShaderResourceView* srv, u32 w, u32 h, const std::string& path) {
  auto* dev = device().dev;
  auto* c = &gpu::rec();
  if (!g_tl_vs) {
    ID3DBlob* vs = gpu::compile_hlsl(kTexLoadHlsl, "vs_main", "vs_5_0");
    ID3DBlob* ps = gpu::compile_hlsl(kTexLoadHlsl, "ps_main", "ps_5_0");
    if (vs) dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_tl_vs);
    if (ps) dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_tl_ps);
    release(vs);
    release(ps);
    if (!g_tl_vs || !g_tl_ps) return false;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* rt = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) return false;
  dev->CreateRenderTargetView(rt, nullptr, &rtv);
  c->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT v{0, 0, float(w), float(h), 0, 1};
  c->RSSetViewports(1, &v);
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_DEPTH_STENCIL_DESC dd{};
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  c->OMSetBlendState(blend_state(bd), nullptr, 0xffffffff);
  c->OMSetDepthStencilState(depth_state(dd), 0);
  c->RSSetState(raster_state(rd));
  c->IASetInputLayout(nullptr);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(g_tl_vs, nullptr, 0);
  c->PSSetShader(g_tl_ps, nullptr, 0);
  c->PSSetShaderResources(0, 1, &srv);
  c->Draw(3, 0);
  ID3D11ShaderResourceView* none = nullptr;
  c->PSSetShaderResources(0, 1, &none);
  ID3D11RenderTargetView* no_rt = nullptr;
  c->OMSetRenderTargets(1, &no_rt, nullptr);
  shadow_reset();
  gpu::rec().finish();  // the draw has run; direct context use until the next flush
  std::vector<u8> rgba;
  u32 rw = 0, rh = 0;
  bool ok = gpu::read_back_texture(rt, rgba, rw, rh) && gpu::write_png(path, rw, rh, rgba);
  release(rtv);
  release(rt);
  return ok;
}

void fr_frame_boundary() {
  if (cfg().frame_report < 0 || g_fr.state == 2) return;
  if (g_fr.state == 0) {
    if (log_time() < cfg().frame_report) return;
    g_fr.state = 1;
    LOG_I("frame", "frame report: frame %llu begins", (unsigned long long)g_frame);
    return;
  }
  g_fr.state = 2;
  LOG_I("frame", "frame report: %u clear(s), %u draw(s), %u dropped; %zu surface(s):", g_fr.clears, g_fr.draws, g_fr.dropped,
        g_fr.touched.size());
  std::filesystem::path dir = std::filesystem::path(cfg().run_dir) / "frame";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (Surface* s : g_fr.touched) {
    std::vector<u8> rgba;
    std::string note;
    std::string name = ac4::strprintf("%s_%s%08x_%ux%u_f%u.png", s->depth ? "z" : "c", s->location ? "m" : "l", s->offset, s->width,
                                       s->height, s->format);
    if (read_surface(s, rgba, &note) && gpu::write_png((dir / name).string(), s->width, s->height, rgba))
      LOG_I("frame", "  %s %s", name.c_str(), note.c_str());
    else
      LOG_I("frame", "  %s: not readable", name.c_str());
  }
  for (auto& t : g_fr.textures) {
    if (!dump_texture(t.srv, t.w, t.h, (dir / t.name).string())) LOG_I("frame", "  %s: not readable", t.name.c_str());
    release(t.srv);
  }
  LOG_I("frame", "  %zu memory texture(s) written", g_fr.textures.size());
  g_fr.textures.clear();
}

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
    rd.SlopeScaledDepthBias = f32_of(reg(m::SET_POLYGON_OFFSET_SCALE_FACTOR));
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
// Vertex cache: converted vertices and indices of array/indexed draws kept in GPU buffers, valid
// while write tracking reports no write to the guest memory they were read from (static meshes
// are converted once instead of every frame).
struct VtxCacheEntry {
  ID3D11Buffer* vb = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11Buffer* ib = nullptr;
  u32 index_count = 0, bytes = 0;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  u32 serial = 0;  // write-tracking serial when built (or when first seen, for a candidate)
  u64 last_frame = 0, seen_frame = ~0ull;  // seen_frame: frame a candidate (no buffers yet) was recorded
  std::vector<std::pair<u32, u32>> ranges;  // guest memory read: [ea, ea + size)
};
std::unordered_map<u64, VtxCacheEntry> g_vcache;
u64 g_vcache_bytes = 0;
constexpr u64 kVcacheBudget = 512ull << 20;

void vcache_release(VtxCacheEntry& e) {
  release(e.srv);
  release(e.vb);
  release(e.ib);
  g_vcache_bytes -= e.bytes;
  e.bytes = 0;
}
// Once the byte budget or the entry limit is exceeded: drops entries unused for 120 frames and
// candidates (no buffers yet) unused for two (called at frame boundaries). While everything is
// still in use, the scan is retried once a second, not every frame.
void vcache_trim() {
  static u64 next_scan = 0;
  if ((g_vcache_bytes <= kVcacheBudget && g_vcache.size() < 50000) || g_frame < next_scan) return;
  next_scan = g_frame + 60;
  for (auto it = g_vcache.begin(); it != g_vcache.end();) {
    if (it->second.last_frame + 120 < g_frame || (!it->second.vb && it->second.last_frame + 2 < g_frame)) {
      vcache_release(it->second);
      it = g_vcache.erase(it);
    } else {
      ++it;
    }
  }
}
bool vcache_valid(const VtxCacheEntry& e) {
  if (!e.vb || !g_track_writes) return false;
  for (auto [ea, size] : e.ranges)
    if (written_since(ea, size, e.serial)) return false;
  return true;
}
// Stores the converted draw; returns false if buffers could not be created.
bool vcache_store(VtxCacheEntry& e, const std::vector<float>& verts, const std::vector<u32>& indices, D3D11_PRIMITIVE_TOPOLOGY topo) {
  vcache_release(e);
  auto* dev = device().dev;
  D3D11_BUFFER_DESC vd{u32(verts.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
  D3D11_SUBRESOURCE_DATA vi{verts.data(), 0, 0};
  D3D11_BUFFER_DESC id{u32(indices.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA ii{indices.data(), 0, 0};
  if (FAILED(dev->CreateBuffer(&vd, &vi, &e.vb)) || FAILED(dev->CreateShaderResourceView(e.vb, nullptr, &e.srv)) ||
      FAILED(dev->CreateBuffer(&id, &ii, &e.ib))) {
    vcache_release(e);
    return false;
  }
  e.index_count = u32(indices.size());
  e.topo = topo;
  e.bytes = vd.ByteWidth + id.ByteWidth;
  g_vcache_bytes += e.bytes;
  e.serial = g_mem_serial;
  for (auto [ea, size] : e.ranges) watch_range(ea, size);
  return true;
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
    if (ppu::r32(ea + 8) == 0) return;  // conditional rendering: report value is zero
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
  gpu::TexDesc tex[16];
  for (u32 u = 0; u < 16; ++u) {
    tex[u] = gpu::decode_texture(g.regs, u);
    if (!tex[u].enabled) continue;
    env.tex_dim[u] = u8(tex[u].cubemap ? 4 : tex[u].dim == 3 ? 3 : tex[u].dim == 1 ? 1 : 2);
  }
  const PsEntry* ps = pixel_shader(env, ::ppu::mem() + fp_ea, max_bytes, code_hash, sp);
  if (!ps->ps) return fr_drop("fragment program unusable");

  // Vertices. The scratch buffers persist across draws: no allocations per draw.
  phase_switch(kPhVertices);
  AttrSrc a[16];
  attr_sources(a);
  u32 mask = vs->info.input_mask;
  // Vertex cache key: everything the converted vertices and indices depend on.
  VtxCacheEntry* vce = nullptr;
  bool vhit = false, cacheable = (clause == Clause::Arrays || clause == Clause::Indexed) && g_imm_vertices.empty();
  u64 vkey = 0;
  if (cacheable) {
    collect_writes();
    u64 h = hash_bytes(reinterpret_cast<const u8*>(g_ranges.data()), g_ranges.size() * sizeof(g_ranges[0]));
    u32 fixed[8] = {u32(clause), prim, mask, reg(m::SET_INDEX_ARRAY_DMA), reg(m::SET_INDEX_ARRAY_ADDRESS),
                    reg(m::SET_VERTEX_DATA_BASE_INDEX), reg(m::SET_RESTART_INDEX_ENABLE), reg(m::SET_RESTART_INDEX)};
    h ^= hash_bytes(reinterpret_cast<const u8*>(fixed), sizeof fixed) * 31;
    for (u32 i = 0; i < 16; ++i) {
      if (!((mask >> i) & 1)) continue;
      if (a[i].array) {
        u32 d[6] = {a[i].type, a[i].size, a[i].stride, a[i].freq, a[i].ea, a[i].modulo};
        h = (h ^ hash_bytes(reinterpret_cast<const u8*>(d), sizeof d)) * 0x9e3779b97f4a7c15ull;
      } else {
        h = (h ^ hash_bytes(reinterpret_cast<const u8*>(g_imm[i]), 16)) * 0x9e3779b97f4a7c15ull;
      }
    }
    vkey = h;
    auto it = g_vcache.find(vkey);
    if (it != g_vcache.end()) {
      vce = &it->second;
      vce->last_frame = g_frame;
    }
    if (vce && vcache_valid(*vce)) {
      ++g_rs.vcached;
      vhit = true;
    } else {
      ++g_rs.vconverted;
      for (u32 i = 0; i < 16; ++i) {  // set by fetch_vertices (other clauses read no arrays by index)
        g_fetch_lo[i] = ~0u;
        g_fetch_hi[i] = 0;
      }
    }
  }
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11ShaderResourceView* draw_srv = g_vsrv;
  ID3D11Buffer* draw_ib = g_ibuf;
  u32 draw_count = 0, start_index = 0;
  if (vhit) {
    topo = vce->topo;
    draw_srv = vce->srv;
    draw_ib = vce->ib;
    draw_count = vce->index_count;
  }
  static std::vector<u32> seq, dense_seq, uniq, positions, indices, slot_of, slot_gen;
  static std::vector<float> verts;
  static std::vector<u8> bytes;
  static u32 gen = 0;
  seq.clear();  // vertex fetch indices in RSX order (~0u = restart)
  verts.clear();
  positions.clear();
  indices.clear();
  if (!vhit) {
  bool restart = reg(m::SET_RESTART_INDEX_ENABLE) != 0;
  u32 restart_value = reg(m::SET_RESTART_INDEX);
  if (clause == Clause::Arrays) {
    for (auto [first, count] : g_ranges)
      for (u32 i = 0; i < count; ++i) seq.push_back(first + i);
  } else if (clause == Clause::Indexed) {
    u32 dma = reg(m::SET_INDEX_ARRAY_DMA);
    bool u16i = ((dma >> 4) & 15) == 1;
    u32 isz = u16i ? 2 : 4;
    u32 base_index = reg(m::SET_VERTEX_DATA_BASE_INDEX);
    u32 iaddr = reg(m::SET_INDEX_ARRAY_ADDRESS) & ~(isz - 1);
    for (auto [first, count] : g_ranges) {
      u32 ea = offset_to_ea(iaddr + first * isz, dma & 15);
      if (!ea) continue;
      for (u32 i = 0; i < count; ++i) {
        u32 v = u16i ? u32(ppu::r16(ea + 2 * i)) : ppu::r32(ea + 4 * i);
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
  if (clause == Clause::Inline) {
    // Interleaved vertices: every enabled array in attribute order, each padded to 32-bit words.
    u32 stride = 0, offs[16] = {};
    for (u32 i = 0; i < 16; ++i)
      if (a[i].size) {
        offs[i] = stride;
        u32 nb = a[i].type == 6 ? 4 : a[i].size * type_size(a[i].type);
        stride += (nb + 3) & ~3u;
      }
    if (!stride) return;
    bytes.resize(g_inline.size() * 4);
    for (size_t i = 0; i < g_inline.size(); ++i) {
      u32 wv = g_inline[i];
      bytes[4 * i] = u8(wv >> 24);
      bytes[4 * i + 1] = u8(wv >> 16);
      bytes[4 * i + 2] = u8(wv >> 8);
      bytes[4 * i + 3] = u8(wv);
    }
    u32 nv = u32(bytes.size() / stride);
    verts.resize(size_t(nv) * 64);
    for (u32 v = 0; v < nv; ++v) {
      for (u32 i = 0; i < 16; ++i) {
        float* o = &verts[size_t(v) * 64 + 4 * i];
        if (a[i].size) convert_attr(a[i].type, a[i].size, bytes.data() + size_t(v) * stride + offs[i], o);
        else std::memcpy(o, g_imm[i], 16);
      }
      seq.push_back(v);
    }
  } else if (!g_imm_vertices.empty()) {
    verts.resize(g_imm_vertices.size() * 64);
    for (size_t v = 0; v < g_imm_vertices.size(); ++v) {
      std::memcpy(&verts[v * 64], g_imm_vertices[v].data(), 256);
      seq.push_back(u32(v));
    }
  } else {
    // Unique fetch indices -> dense vertex array, via a generation-stamped table over the
    // draw's index range (indices are at most 20 bits).
    u32 lo = ~0u, hi = 0;
    for (u32 k : seq)
      if (k != ~0u) {
        lo = std::min(lo, k);
        hi = std::max(hi, k);
      }
    if (lo == ~0u) return fr_drop("no vertices");
    u32 span = hi - lo + 1;
    if (slot_gen.size() < span) {
      slot_gen.resize(span, 0);
      slot_of.resize(span);
    }
    if (++gen == 0) {
      std::fill(slot_gen.begin(), slot_gen.end(), 0u);
      gen = 1;
    }
    uniq.clear();
    dense_seq.clear();
    dense_seq.reserve(seq.size());
    for (u32 k : seq) {
      if (k == ~0u) {
        dense_seq.push_back(~0u);
        continue;
      }
      u32 i = k - lo;
      if (slot_gen[i] != gen) {
        slot_gen[i] = gen;
        slot_of[i] = u32(uniq.size());
        uniq.push_back(k);
      }
      dense_seq.push_back(slot_of[i]);
    }
    verts.resize(uniq.size() * 64);
    fetch_vertices(a, mask, uniq, lo, hi, verts.data());
    g_rs.vertices += uniq.size();
    g_rs.max_draw_vertices = std::max<u64>(g_rs.max_draw_vertices, uniq.size());
    seq.swap(dense_seq);
  }
  if (verts.empty()) return fr_drop("no vertices");
  topo = convert_primitive(prim, seq, &positions);
  if (topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED || positions.empty()) return fr_drop("primitive not drawable");
  indices.reserve(positions.size());
  for (u32 p : positions) indices.push_back(seq[p]);
  draw_count = u32(indices.size());
  bool stored = false;
  // Only larger draws whose source stayed unwritten since an earlier frame get buffers of their
  // own; one-off and per-frame data goes through the ring (buffer creation is costly).
  if (cacheable && verts.size() / 64 < 128) {
    if (vce) g_vcache.erase(vkey);
    vce = nullptr;
  } else if (cacheable) {  // keep the converted draw: the guest memory it came from is watched
    if (!vce) {
      vce = &g_vcache[vkey];
      vce->last_frame = g_frame;
    }
    vce->ranges.clear();
    for (u32 i = 0; i < 16; ++i)
      if (((mask >> i) & 1) && a[i].array && g_fetch_lo[i] <= g_fetch_hi[i]) {
        u32 nb = a[i].type == 6 ? 4 : a[i].size * type_size(a[i].type);
        u64 b = u64(a[i].ea) + u64(g_fetch_lo[i]) * a[i].stride, e = u64(a[i].ea) + u64(g_fetch_hi[i]) * a[i].stride + nb;
        if (e > b && e <= 0x100000000ull) vce->ranges.push_back({u32(b), u32(e - b)});
      }
    if (clause == Clause::Indexed) {
      u32 dma = reg(m::SET_INDEX_ARRAY_DMA);
      u32 isz = ((dma >> 4) & 15) == 1 ? 2 : 4;
      u32 iaddr = reg(m::SET_INDEX_ARRAY_ADDRESS) & ~(isz - 1);
      for (auto [first, count] : g_ranges)
        if (u32 ea = offset_to_ea(iaddr + first * isz, dma & 15)) vce->ranges.push_back({ea, count * isz});
    }
    if (vce->seen_frame == ~0ull || vce->seen_frame == g_frame || vce->vb) {
      vcache_release(*vce);
      vce->seen_frame = g_frame;
      vce->serial = g_mem_serial;
      for (auto [ea, size] : vce->ranges) watch_range(ea, size);
    } else {
      bool unchanged = true;
      for (auto [ea, size] : vce->ranges) unchanged = unchanged && !written_since(ea, size, vce->serial);
      if (unchanged) {
        stored = vcache_store(*vce, verts, indices, topo);
      } else {
        vce->serial = g_mem_serial;
        for (auto [ea, size] : vce->ranges) watch_range(ea, size);
      }
    }
    if (stored) {
      draw_srv = vce->srv;
      draw_ib = vce->ib;
    }
  }
  if (!stored) {
    // The ring keeps vertices 16-float4 aligned: pad the position up to a vertex boundary.
    if (g_vbuf_pos % 16) g_vbuf_pos += 16 - g_vbuf_pos % 16;
    // The vertex shader fetches by SV_VertexID: the ring position goes into the indices.
    phase_switch(kPhUploads);
    u32 first_vertex = upload_vertices(verts);
    if (first_vertex)
      for (u32& i : indices) i += first_vertex;
    start_index = upload_indices(indices);
    draw_srv = g_vsrv;
    draw_ib = g_ibuf;
  }
  }  // !vhit

  phase_switch(kPhState);
  auto* c = &gpu::rec();
  set_pipeline_state(t);
  phase_switch(kPhConstants);
  // Constant buffers.
  if (g_vc_dirty) {
    u32 lo = g_vc_lo & ~3u, hi = std::min<u32>((g_vc_hi + 3) & ~3u, 512 * 4);  // whole constants
    if (hi > lo) c->UpdateConstants(g_vc_buf, lo * 4, (hi - lo) * 4, &g_vc[lo]);
    g_vc_dirty = false;
    g_vc_lo = 512 * 4;
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
  fst.alpha[2] = 1.0f / float(t.sx);  // window position: storage pixels -> pixels
  fst.alpha[3] = 1.0f / float(t.sy);
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
    static bool fc_valid = false;
    if (!fc_valid || std::memcmp(fc, fc_last, size_t(n) * 16)) {
      c->UpdateSubresource(g_fp_const, 0, nullptr, fc, 0, 0, sizeof fc);  // constant buffers update as a whole
      std::memcpy(fc_last, fc, sizeof fc);
      fc_valid = true;
    }
  }
  phase_switch(kPhDraws);
  if (!g_sh.fixed) {
    ID3D11Buffer* vcb[2] = {g_vc_buf, g_vs_state};
    c->VSSetConstantBuffers(0, 2, vcb);
    ID3D11Buffer* pcb[2] = {g_fp_state, g_fp_const};
    c->PSSetConstantBuffers(2, 2, pcb);
    c->IASetInputLayout(nullptr);
    g_sh.fixed = 1;
  }
  if (g_sh.vs_srv != draw_srv) {
    c->VSSetShaderResources(0, 1, &draw_srv);
    g_sh.vs_srv = draw_srv;
  }
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
  if (g_sh.topo != u32(topo)) {
    c->IASetPrimitiveTopology(topo);
    g_sh.topo = u32(topo);
  }
  if (g_sh.ib != draw_ib) {
    c->IASetIndexBuffer(draw_ib, DXGI_FORMAT_R32_UINT, 0);
    g_sh.ib = draw_ib;
  }
  if (gpu::timing::per_draw())
    gpu::timing::mark(ac4::strprintf("%s fp %08x vp %08x", g_rt_label.c_str(), u32(code_hash), u32(reinterpret_cast<uintptr_t>(vs->vs) >> 4)));
  c->DrawIndexed(draw_count, start_index, 0);
  if (gpu::timing::per_draw()) gpu::timing::mark(g_rt_label + " between draws");
  gpu::rec().kick();
  ++g_rs.draws;
  ++g_write_serial;
  for (u32 i = 0; i < t.count; ++i) t.color[i]->written = g_write_serial;
  if (depth_changed(t)) t.depth->written = g_write_serial;
  if (fr_on()) {
    ++g_fr.draws;
    for (u32 i = 0; i < t.count; ++i) fr_touch(t.color[i]);
    fr_touch(t.depth);
    std::string tx;
    for (u32 u = 0; u < 16; ++u) {
      if (!((ps->info.tex_mask >> u) & 1) || !tex[u].enabled) continue;
      Surface* ts = texture_surface(tex[u]);
      fr_touch(ts);
      if (!ts) fr_texture(srvs[u], tex[u]);
      tx += ac4::strprintf(" t%u=%s%x/f%02x%s/%ux%u/%s/r%04x%s", u, tex[u].location ? "M:" : "", tex[u].offset, tex[u].format,
                            tex[u].linear ? "ln" : "", tex[u].width, tex[u].height, ts ? "surf" : "mem", tex[u].remap & 0xffff,
                            srvs[u] ? "" : "(unbound)");
    }
    LOG_I("frame", "draw %u prim %u verts %zu idx %zu %s vp %u fp %08x zt %d/%x/%d blend %d cull %x%s", g_fr.draws, prim, verts.size() / 64,
          indices.size(), targets_desc(t).c_str(), reg(m::SET_TRANSFORM_PROGRAM_START), sp, reg(m::SET_DEPTH_TEST_ENABLE) ? 1 : 0,
          reg(m::SET_DEPTH_FUNC), reg(m::SET_DEPTH_MASK) ? 1 : 0, reg(m::SET_BLEND_ENABLE) ? 1 : 0,
          reg(m::SET_CULL_FACE_ENABLE) ? reg(m::SET_CULL_FACE) : 0, tx.c_str());
    if (verts.size() / 64 <= 64) {  // small draws (particles, HUD): vertex inputs as fetched
      std::string in;
      for (u32 i = 0; i < 16; ++i) {
        if (!((mask >> i) & 1)) continue;
        in += a[i].array ? ac4::strprintf(" i%u:t%u s%u st%u", i, a[i].type, a[i].size, a[i].stride) : ac4::strprintf(" i%u:imm", i);
        for (size_t v = 0; v < std::min<size_t>(verts.size() / 64, 2); ++v)
          in += ac4::strprintf(" (%g %g %g %g)", verts[v * 64 + 4 * i], verts[v * 64 + 4 * i + 1], verts[v * 64 + 4 * i + 2],
                                verts[v * 64 + 4 * i + 3]);
      }
      LOG_I("frame", "  inputs:%s", in.c_str());
    }
  }
}

void fr_clear(const Targets& t, u32 mask) {
  if (!fr_on()) return;
  ++g_fr.clears;
  for (u32 i = 0; i < t.count; ++i) fr_touch(t.color[i]);
  fr_touch(t.depth);
  LOG_I("frame", "clear mask 0x%x %s color 0x%08x z 0x%08x", mask, targets_desc(t).c_str(), reg(m::SET_COLOR_CLEAR_VALUE),
        reg(m::SET_ZSTENCIL_CLEAR_VALUE));
}

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

// Captures do not stall the frame: the copy is read back once the GPU has finished it and the PNG
// is encoded on a worker thread.
void maybe_capture() {
  gpu::rec().call([](ID3D11DeviceContext*) {  // in stream order, after the frame's presentation
    gpu::poll_captures([](u32 n, std::vector<u8>&& rgba, u32 w, u32 h) {
      std::thread([n, w, h, px = std::move(rgba)] {
        std::string path = cfg().run_dir + ac4::strprintf("\\shot_%04u.png", n);
        bool uniform = !px.empty();  // a frame of one color usually means nothing was drawn: say so
        for (size_t i = 4; i < px.size() && uniform; i += 4) uniform = std::memcmp(&px[i], &px[0], 4) == 0;
        if (gpu::write_png(path, w, h, px))
          LOG_I("gpu", "captured %s (%ux%u%s)", path.c_str(), w, h,
                uniform ? ac4::strprintf(", uniform color %02x%02x%02x", px[0], px[1], px[2]).c_str() : "");
      }).detach();
    });
  });
  if (gpu::g_capture_requests.load() <= 0) return;
  gpu::g_capture_requests.fetch_sub(1);
  u32 n = gpu::g_capture_index.fetch_add(1);
  gpu::rec().call([n](ID3D11DeviceContext*) { gpu::begin_capture(n); });
  std::vector<u8> rgba;
  u32 w, h;
  if (cfg().shot_surface >= 0) {  // diagnostics: an intermediate render target (read back at once)
    gpu::rec().finish();
    Surface* s = find_surface(0, u32(cfg().shot_surface));
    if (s && !s->depth && s->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM && gpu::read_back_texture(s->tex, rgba, w, h)) {
      std::string sp = cfg().run_dir + ac4::strprintf("\\shot_%04u_surface_%lx.png", n, cfg().shot_surface);
      gpu::write_png(sp, w, h, rgba);
    } else {
      LOG_I("gpu", "surface 0x%lx: %s", cfg().shot_surface, s ? "not a readable RGBA8 color surface" : "none");
    }
  }
}

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
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  u64 now = u64(t.QuadPart);
  if (g_phase_at) g_phase_ticks[g_phase] += now - g_phase_at;
  g_phase_at = now;
  int prev = g_phase;
  g_phase = p;
  return prev;
}

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
  static double ms_per_tick = 0;
  if (!ms_per_tick) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    ms_per_tick = 1000.0 / double(f.QuadPart);
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
  vcache_trim();
  tex_trim();
  surface_trim();
  g_writes_stale = true;
  if (g_frame && g_frame % 300 == 0) {
    auto pf = [](u64 RenderStats::*f) { return double(g_rs.*f - g_rs_last.*f) / 300.0; };
    u64 surf_bytes = 0;
    for (const Surface* s : g_surfaces) surf_bytes += surface_bytes(s);
    LOG_I("perf",
          "per frame: %.0f draws (vertices %.0f cached, %.0f converted), textures %.1f hashed %.1f uploaded, %.2f surfaces created, "
          "%.1f target changes, %.1f write queries (%.0f MiB); vertex cache %zu entries %.0f MiB; textures %zu %.0f MiB; "
          "surfaces %zu %.0f MiB",
          pf(&RenderStats::draws), pf(&RenderStats::vcached), pf(&RenderStats::vconverted), pf(&RenderStats::tex_hashed),
          pf(&RenderStats::tex_uploaded), pf(&RenderStats::surfaces), pf(&RenderStats::target_binds), pf(&RenderStats::collections),
          pf(&RenderStats::bytes_queried) / 1048576.0, g_vcache.size(), g_vcache_bytes / 1048576.0, g_tex.size(),
          g_tex_bytes / 1048576.0, g_surfaces.size(), surf_bytes / 1048576.0);
    g_rs_last = g_rs;
  }
  ++g_frame;
  gpu::timing::mark("present");
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
  gpu::rec().frame_end(1);  // at most one presented frame queued behind the FIFO thread
  gpu::apply_submit_mode();
}

void renderer_constants(u32 first, const u8* be, u32 count) {
  u32 word = g_vc_load * 4 + first, n = 0;
  for (; n < count && word + n < 468 * 4; ++n) {
    u32 v;
    std::memcpy(&v, be + 4 * n, 4);
    v = __builtin_bswap32(v);
    std::memcpy(&g_vc[word + n], &v, 4);
  }
  if (n) vc_mark(word, n);
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
    if (word < 468 * 4) {
      std::memcpy(&g_vc[word], &arg, 4);
      vc_mark(word, 1);
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

bool gpu_option(const std::string& name, const std::string& value) {
  if (name == "submit" && (value == "on" || value == "off")) {
    gpu::request_submit_mode(value == "on");
    LOG_I("gpu", "D3D11 submission thread requested %s (applied at the next frame)", value.c_str());
    return true;
  }
  return false;
}

u32 renderer_report_value(u32 type) {
  warn_once("report", "GET_REPORT type " + std::to_string(type) + " answered with 0 (occlusion/zcull counting not implemented)");
  return 0;
}

}  // namespace rt::rsx
