#include "runtime/gpu/d3d11_device.h"

#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <set>

#include "common/crypto.h"
#include "common/file.h"
#include "runtime/config.h"
#include "runtime/gpu/display.h"
#include "runtime/log.h"
#include "runtime/ui/overlay.h"

namespace rt::gpu {
namespace {

Device g_dev;
HWND g_hwnd = nullptr;
bool g_tearing = false;  // the swap chain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
ID3D11VertexShader* g_blit_vs = nullptr;
ID3D11PixelShader* g_blit_ps = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11Texture2D* g_last = nullptr;  // copy of the last presented frame (for captures)
u32 g_last_w = 0, g_last_h = 0;
ID3D11Texture2D* g_ui_copy = nullptr;  // the composed back buffer while PC SETTINGS is open (captures)
u32 g_ui_w = 0, g_ui_h = 0;
// Exclusive fullscreen: requested by any thread, carried out on the presenting thread.
std::atomic<int> g_excl_request{-1};  // -1 none, 0 leave, 1 enter
std::atomic<u32> g_excl_w{0}, g_excl_h{0}, g_excl_hz{0};
std::atomic<u32> g_mode_w{0}, g_mode_h{0};  // the display mode exclusive fullscreen runs in
std::atomic<bool> g_exclusive{false};
bool g_excl_wanted = false;  // presenting thread: the mode is fullscreen (re-entered after Alt+Tab)
std::chrono::steady_clock::time_point g_excl_retry;

const char* kBlitHlsl = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target { return float4(t0.Sample(s0, i.uv).rgb, 1); }
)";

}  // namespace

Device& device() { return g_dev; }

// Compiled shaders are kept in the machine-local shader cache, keyed by the SHA-256 of the
// source, entry point, profile and compiler settings: a shader compiles once per installation.
namespace {
std::atomic<u64> g_compile_us{0};
std::atomic<u32> g_compiles{0}, g_cache_hits{0};
}  // namespace
ID3DBlob* compile_hlsl(const std::string& src, const char* entry, const char* target) {
  const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  std::string key_src = std::string(target) + "|" + entry + "|" + std::to_string(flags) + "|" + src;
  auto h = ac4::crypto::sha256(key_src.data(), key_src.size());
  std::string name;
  for (int i = 0; i < 16; ++i) name += ac4::strprintf("%02x", h[i]);
  std::filesystem::path file;
  if (!cfg().shader_cache.empty()) {
    file = std::filesystem::path(cfg().shader_cache) / (name + ".dxbc");
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
      try {
        std::vector<u8> bin = ac4::read_file(file);
        ID3DBlob* blob = nullptr;
        if (!bin.empty() && SUCCEEDED(D3DCreateBlob(bin.size(), &blob))) {
          std::memcpy(blob->GetBufferPointer(), bin.data(), bin.size());
          g_cache_hits.fetch_add(1);
          return blob;
        }
      } catch (...) {
      }
    }
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  auto t0 = std::chrono::steady_clock::now();
  HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, flags, 0, &code, &err);
  u64 us = u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  u64 total = g_compile_us.fetch_add(us) + us;
  u32 n = g_compiles.fetch_add(1) + 1;
  if (us > 100000)
    LOG_I("gpu", "slow shader compile: %.0f ms (%s); %u compiled this run, %.1f s in total", us / 1000.0, target, n, total / 1e6);
  if (FAILED(hr)) {
    LOG_E("gpu", "HLSL compile failed (%s/%s): %s", entry, target, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    if (err) err->Release();
    return nullptr;
  }
  if (err) err->Release();
  if (!file.empty()) {
    try {
      ac4::write_file(file, code->GetBufferPointer(), code->GetBufferSize());
    } catch (const std::exception& e) {
      LOG_W("gpu", "shader cache: %s", e.what());
    }
  }
  return code;
}

void shader_stats(u32* compiled, u32* cached, double* seconds) {
  *compiled = g_compiles.load();
  *cached = g_cache_hits.load();
  *seconds = g_compile_us.load() / 1e6;
}

bool device_init(void* hwnd) {
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                 D3D11_SDK_VERSION, &g_dev.dev, &got, &g_dev.ctx);
  if (FAILED(hr)) {
    LOG_E("gpu", "D3D11CreateDevice failed 0x%08lx", hr);
    return false;
  }
  IDXGIDevice* dxdev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  g_dev.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxdev));
  dxdev->GetAdapter(&adapter);
  DXGI_ADAPTER_DESC ad{};
  adapter->GetDesc(&ad);
  char name[128];
  WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
  D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
  if (SUCCEEDED(g_dev.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof opt)))
    g_dev.no_overwrite_srv = opt.MapNoOverwriteOnDynamicBufferSRV;
  LOG_I("gpu", "D3D11 device on '%s' (%llu MB VRAM); no-overwrite maps of shader-resource buffers %s", name,
        (unsigned long long)(ad.DedicatedVideoMemory >> 20), g_dev.no_overwrite_srv ? "supported" : "not supported");
  if (hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    if (rc.right > 0 && rc.bottom > 0) g_dev.width = u32(rc.right), g_dev.height = u32(rc.bottom);
    IDXGIFactory2* fac = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac));
    // Tearing support (DXGI 1.5): needed to present at the game's own rate on variable-refresh
    // displays (gpu/display.h).
    IDXGIFactory5* fac5 = nullptr;
    BOOL tearing = FALSE;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&fac5)))) {
      if (FAILED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE;
      fac5->Release();
    }
    g_tearing = tearing;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_dev.width;
    sd.Height = g_dev.height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = fac->CreateSwapChainForHwnd(g_dev.dev, g_hwnd, &sd, nullptr, nullptr, &g_dev.swap);
    fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);  // the window handles Alt+Enter (borderless fullscreen)
    fac->Release();
    if (FAILED(hr)) {
      LOG_E("gpu", "CreateSwapChainForHwnd failed 0x%08lx", hr);
      return false;
    }
    ID3D11Texture2D* bb = nullptr;
    g_dev.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    g_dev.dev->CreateRenderTargetView(bb, nullptr, &g_dev.backbuffer_rtv);
    bb->Release();
    LOG_I("gpu", "swap chain %ux%u, flip model, tearing %s", g_dev.width, g_dev.height, g_tearing ? "supported" : "not supported");
    display::swapchain_created(g_dev.swap, g_tearing);
  }
  adapter->Release();
  dxdev->Release();
  ID3DBlob* vs = compile_hlsl(kBlitHlsl, "vs_main", "vs_5_0");
  ID3DBlob* ps = compile_hlsl(kBlitHlsl, "ps_main", "ps_5_0");
  if (!vs || !ps) return false;
  g_dev.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_blit_vs);
  g_dev.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_blit_ps);
  vs->Release();
  ps->Release();
  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  g_dev.dev->CreateSamplerState(&smp, &g_linear);
  return true;
}

// Logs why a creation call failed (once per kind); a removed device is reported with its reason.
void report_failure(const char* what, HRESULT hr) {
  static std::mutex mu;
  static std::set<std::string> seen;
  std::lock_guard lk(mu);
  if (!seen.insert(what).second) return;
  HRESULT removed = g_dev.dev->GetDeviceRemovedReason();
  LOG_E("gpu", "%s failed (0x%08lx)%s", what, (unsigned long)hr,
        removed != S_OK ? ac4::strprintf("; the D3D11 device was removed (reason 0x%08lx)", (unsigned long)removed).c_str() : "");
}

// The swap chain follows the window's client size (resizing, borderless fullscreen), checked
// before each present on the presenting thread; in exclusive fullscreen it has the display mode's
// size (the window's client area passes through other sizes while DXGI switches the mode).
void resize_to_window() {
  RECT rc{};
  if (!g_hwnd || !GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;  // minimized: keep the buffers
  u32 w = u32(rc.right), h = u32(rc.bottom);
  if (g_exclusive.load() && g_mode_w.load() && g_mode_h.load()) {
    static u32 noted_w = 0, noted_h = 0;
    if ((w != g_mode_w || h != g_mode_h) && (w != noted_w || h != noted_h)) {
      noted_w = w, noted_h = h;
      LOG_I("gpu", "exclusive fullscreen: the window's client area is %ux%u, the display mode %ux%u", w, h, g_mode_w.load(),
            g_mode_h.load());
    }
    w = g_mode_w, h = g_mode_h;
  }
  if (w == g_dev.width && h == g_dev.height) return;
  g_dev.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  if (g_dev.backbuffer_rtv) g_dev.backbuffer_rtv->Release();
  g_dev.backbuffer_rtv = nullptr;
  HRESULT hr = g_dev.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr)) report_failure("swap chain resize", hr);
  else g_dev.width = w, g_dev.height = h;
  ID3D11Texture2D* bb = nullptr;
  g_dev.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
  if (bb) {
    g_dev.dev->CreateRenderTargetView(bb, nullptr, &g_dev.backbuffer_rtv);
    bb->Release();
  }
  LOG_I("gpu", "swap chain resized to %ux%u", g_dev.width, g_dev.height);
}

namespace {
// Presenting thread. DXGI sends the window messages while switching; the window thread keeps
// pumping them (it never waits for this thread).
void enter_exclusive() {
  IDXGIOutput* out = nullptr;
  if (FAILED(g_dev.swap->GetContainingOutput(&out)) || !out) return;
  DXGI_OUTPUT_DESC od{};
  out->GetDesc(&od);
  DXGI_MODE_DESC want{};
  want.Width = g_excl_w ? g_excl_w.load() : u32(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
  want.Height = g_excl_h ? g_excl_h.load() : u32(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
  u32 hz = g_excl_hz.load();
  if (!hz) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) hz = dm.dmDisplayFrequency;
  }
  want.RefreshRate = DXGI_RATIONAL{hz, hz ? 1u : 0u};
  want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  DXGI_MODE_DESC mode{};
  if (FAILED(out->FindClosestMatchingMode(&want, &mode, g_dev.dev))) mode = want;
  g_dev.swap->ResizeTarget(&mode);
  HRESULT hr = g_dev.swap->SetFullscreenState(TRUE, out);
  out->Release();
  if (FAILED(hr)) {  // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE while another window is in front: tried again
    g_excl_retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    LOG_I("gpu", "exclusive fullscreen not available now (0x%08lx); trying again when the window is in front", (unsigned long)hr);
    return;
  }
  mode.RefreshRate = DXGI_RATIONAL{0, 0};  // as DXGI recommends after the switch
  g_dev.swap->ResizeTarget(&mode);
  g_mode_w = mode.Width;
  g_mode_h = mode.Height;
  g_exclusive = true;
  display::set_exclusive(true);
  LOG_I("gpu", "exclusive fullscreen %ux%u at %.2f Hz", mode.Width, mode.Height,
        want.RefreshRate.Denominator ? double(want.RefreshRate.Numerator) / want.RefreshRate.Denominator : 0.0);
}

void leave_exclusive() {
  g_dev.swap->SetFullscreenState(FALSE, nullptr);
  g_exclusive = false;
  display::set_exclusive(false);
  LOG_I("gpu", "exclusive fullscreen left");
}

void update_exclusive() {
  int req = g_excl_request.exchange(-1);
  if (req == 0) {
    g_excl_wanted = false;
    if (g_exclusive) leave_exclusive();
    PostMessageW(g_hwnd, WM_APP + 1, 0, 0);  // the window applies its own mode (window.cpp)
  } else if (req == 1) {
    g_excl_wanted = true;
    if (g_exclusive) leave_exclusive();  // another display mode
    enter_exclusive();
  } else if (g_excl_wanted) {
    BOOL fs = FALSE;
    if (g_exclusive && SUCCEEDED(g_dev.swap->GetFullscreenState(&fs, nullptr)) && !fs) {  // DXGI left it (Alt+Tab)
      g_exclusive = false;
      display::set_exclusive(false);
      LOG_I("gpu", "exclusive fullscreen lost; entered again when the window is in front");
    }
    if (!g_exclusive && GetForegroundWindow() == g_hwnd && std::chrono::steady_clock::now() >= g_excl_retry) enter_exclusive();
  }
}

}  // namespace

void request_exclusive(bool on, u32 w, u32 h, u32 hz) {
  g_excl_w = w;
  g_excl_h = h;
  g_excl_hz = hz;
  g_excl_request = on ? 1 : 0;
}
bool exclusive_active() { return g_exclusive.load(); }
bool exclusive_mode_size(int* w, int* h) {
  if (!g_exclusive.load() || !g_mode_w.load() || !g_mode_h.load()) return false;
  *w = int(g_mode_w.load());
  *h = int(g_mode_h.load());
  return true;
}

void present_texture(ID3D11ShaderResourceView* srv, u32 src_w, u32 src_h) {
  auto* c = g_dev.ctx;
  // Keep a copy for captures (same size as the source).
  if (!g_last || g_last_w != src_w || g_last_h != src_h) {
    if (g_last) g_last->Release();
    g_last = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = src_w;
    td.Height = src_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (HRESULT hr = g_dev.dev->CreateTexture2D(&td, nullptr, &g_last); FAILED(hr)) {
      report_failure("presentation copy texture", hr);
      g_last = nullptr;
      return;
    }
    g_last_w = src_w;
    g_last_h = src_h;
  }
  auto draw_to = [&](ID3D11RenderTargetView* rtv, u32 w, u32 h, const D3D11_VIEWPORT* fit = nullptr) {
    D3D11_VIEWPORT vp = fit ? *fit : D3D11_VIEWPORT{0, 0, float(w), float(h), 0, 1};
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0);
    c->RSSetState(nullptr);
    c->RSSetViewports(1, &vp);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g_blit_vs, nullptr, 0);
    c->PSSetShader(g_blit_ps, nullptr, 0);
    c->PSSetShaderResources(0, 1, &srv);
    c->PSSetSamplers(0, 1, &g_linear);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
  };
  ID3D11RenderTargetView* last_rtv = nullptr;
  if (HRESULT hr = g_dev.dev->CreateRenderTargetView(g_last, nullptr, &last_rtv); FAILED(hr) || !last_rtv) {
    report_failure("presentation render target view", hr);
    return;
  }
  draw_to(last_rtv, src_w, src_h);
  last_rtv->Release();
  if (g_dev.swap) {
    update_exclusive();
    resize_to_window();
    // Aspect-correct fit: the frame is scaled to the largest centred rectangle with its own
    // aspect ratio; the rest of the window stays black.
    float sx = float(g_dev.width) / float(src_w), sy = float(g_dev.height) / float(src_h), s = std::min(sx, sy);
    float vw = std::floor(float(src_w) * s + 0.5f), vh = std::floor(float(src_h) * s + 0.5f);
    D3D11_VIEWPORT fit{std::floor((float(g_dev.width) - vw) / 2), std::floor((float(g_dev.height) - vh) / 2), vw, vh, 0, 1};
    const float black[4] = {0, 0, 0, 1};
    c->ClearRenderTargetView(g_dev.backbuffer_rtv, black);
    draw_to(g_dev.backbuffer_rtv, g_dev.width, g_dev.height, &fit);
    if (ui::screen_up()) {  // PC SETTINGS or a system dialog over the game's frame, at the window's resolution
      ID3D11Texture2D* bb = nullptr;
      g_dev.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
      if (bb) {
        c->OMSetRenderTargets(0, nullptr, nullptr);
        ui::render(g_dev.dev, c, bb, g_dev.width, g_dev.height);
        if (!g_ui_copy || g_ui_w != g_dev.width || g_ui_h != g_dev.height) {
          if (g_ui_copy) g_ui_copy->Release();
          g_ui_copy = nullptr;
          D3D11_TEXTURE2D_DESC td{};
          bb->GetDesc(&td);
          td.BindFlags = 0;
          td.MiscFlags = 0;
          td.Usage = D3D11_USAGE_DEFAULT;
          if (SUCCEEDED(g_dev.dev->CreateTexture2D(&td, nullptr, &g_ui_copy))) g_ui_w = g_dev.width, g_ui_h = g_dev.height;
        }
        if (g_ui_copy) c->CopyResource(g_ui_copy, bb);
        bb->Release();
      }
    }
    g_dev.swap->Present(0, display::present_flags());
    display::after_present(g_dev.swap);
  }
}

bool read_back_last_frame(std::vector<u8>& rgba, u32& w, u32& h) { return read_back_texture(g_last, rgba, w, h); }

struct PendingCapture {
  ID3D11Texture2D* staging = nullptr;
  u32 index = 0, w = 0, h = 0;
  bool bgra = false;
};
std::vector<PendingCapture> g_captures;

void begin_capture(u32 index) {
  // While PC SETTINGS is open the capture shows the window as composed (the screen included).
  ID3D11Texture2D* src = ui::screen_up() && g_ui_copy ? g_ui_copy : g_last;
  if (!src || g_captures.size() >= 8) return;
  D3D11_TEXTURE2D_DESC td{};
  src->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  PendingCapture c;
  if (HRESULT hr = g_dev.dev->CreateTexture2D(&td, nullptr, &c.staging); FAILED(hr)) {
    report_failure("capture staging texture", hr);
    return;
  }
  g_dev.ctx->CopyResource(c.staging, src);
  c.index = index;
  c.w = td.Width;
  c.h = td.Height;
  c.bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
  g_captures.push_back(c);
}

void poll_captures(const std::function<void(u32 index, std::vector<u8>&& rgba, u32 w, u32 h)>& done) {
  for (auto it = g_captures.begin(); it != g_captures.end();) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    HRESULT hr = g_dev.ctx->Map(it->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {  // not finished yet: try again next frame
      ++it;
      continue;
    }
    if (SUCCEEDED(hr)) {
      std::vector<u8> rgba(size_t(it->w) * it->h * 4);
      for (u32 y = 0; y < it->h; ++y)
        std::memcpy(&rgba[size_t(y) * it->w * 4], static_cast<u8*>(ms.pData) + size_t(y) * ms.RowPitch, size_t(it->w) * 4);
      g_dev.ctx->Unmap(it->staging, 0);
      if (it->bgra)
        for (size_t i = 0; i < rgba.size(); i += 4) {
          std::swap(rgba[i], rgba[i + 2]);
          rgba[i + 3] = 0xff;
        }
      done(it->index, std::move(rgba), it->w, it->h);
    } else {
      report_failure("capture readback", hr);
    }
    it->staging->Release();
    it = g_captures.erase(it);
  }
}

bool read_back_texture(ID3D11Texture2D* tex, std::vector<u8>& rgba, u32& w, u32& h) {
  if (!tex) return false;
  D3D11_TEXTURE2D_DESC td{};
  tex->GetDesc(&td);
  if (td.SampleDesc.Count != 1 || td.ArraySize != 1) return false;
  td.MipLevels = 1;
  td.MiscFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(g_dev.dev->CreateTexture2D(&td, nullptr, &st))) return false;
  g_dev.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (FAILED(g_dev.ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
    st->Release();
    return false;
  }
  w = td.Width;
  h = td.Height;
  rgba.resize(size_t(w) * h * 4);
  for (u32 y = 0; y < h; ++y) std::memcpy(&rgba[size_t(y) * w * 4], static_cast<u8*>(ms.pData) + size_t(y) * ms.RowPitch, w * 4);
  g_dev.ctx->Unmap(st, 0);
  st->Release();
  return true;
}

}  // namespace rt::gpu
