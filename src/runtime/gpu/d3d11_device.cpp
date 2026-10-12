#include "runtime/gpu/d3d11_device.h"

#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>

#include "common/crypto.h"
#include "common/file.h"
#include "runtime/config.h"
#include "runtime/gpu/png.h"
#include "runtime/gpu/present.h"
#include "runtime/log.h"

namespace rt::gpu {
namespace {

Device g_dev;
ID3D11VertexShader* g_blit_vs = nullptr;
ID3D11PixelShader* g_blit_ps = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11Texture2D* g_last = nullptr;  // copy of the last presented frame (for captures)
u32 g_last_w = 0, g_last_h = 0;
HWND g_hwnd = nullptr;
HANDLE g_latency_wait = nullptr;  // the swap chain's frame latency waitable object
UINT g_swap_flags = 0;
bool g_tearing = false;           // DXGI_PRESENT_ALLOW_TEARING supported
std::atomic<u32> g_interval{1};   // sync interval of the next presents (set_present_interval)
std::atomic<int> g_output_shots{0};

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
    IDXGIFactory2* fac = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac));
    // Tearing (sync interval 0 shown at once: no limit without vsync, or a variable-refresh display
    // in borderless fullscreen) needs the system's support.
    IDXGIFactory5* fac5 = nullptr;
    BOOL tearing = FALSE;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&fac5)))) {
      if (FAILED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE;
      fac5->Release();
    }
    g_tearing = tearing;
    g_swap_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    RECT cr{};
    if (GetClientRect(g_hwnd, &cr) && cr.right > cr.left && cr.bottom > cr.top) {
      g_dev.width = u32(cr.right - cr.left);
      g_dev.height = u32(cr.bottom - cr.top);
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_dev.width;
    sd.Height = g_dev.height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_swap_flags;
    hr = fac->CreateSwapChainForHwnd(g_dev.dev, g_hwnd, &sd, nullptr, nullptr, &g_dev.swap);
    fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    fac->Release();
    if (FAILED(hr)) {
      LOG_E("gpu", "CreateSwapChainForHwnd failed 0x%08lx", hr);
      return false;
    }
    // At most one frame waits for the display (present.h wait_present_ready): low latency, and
    // flips can follow the display's refresh.
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(g_dev.swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2)))) {
      sc2->SetMaximumFrameLatency(1);
      g_latency_wait = sc2->GetFrameLatencyWaitableObject();
      sc2->Release();
    }
    LOG_I("gpu", "swap chain %ux%u (flip model, 3 buffers, maximum frame latency 1%s); tearing %s", g_dev.width, g_dev.height,
          g_latency_wait ? "" : " NOT available", g_tearing ? "supported" : "not supported");
    ID3D11Texture2D* bb = nullptr;
    g_dev.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    g_dev.dev->CreateRenderTargetView(bb, nullptr, &g_dev.backbuffer_rtv);
    bb->Release();
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

void request_output_capture() { g_output_shots.fetch_add(1); }

void set_present_interval(u32 k) { g_interval.store(std::min<u32>(k, 4), std::memory_order_relaxed); }

bool wait_present_ready(u32 timeout_ms) {
  if (!g_latency_wait) return false;
  return WaitForSingleObjectEx(g_latency_wait, timeout_ms, TRUE) == WAIT_OBJECT_0;
}

namespace {

// The back buffer as presented (read back at once: diagnostics only).
void capture_output() {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_dev.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
  std::vector<u8> px;
  u32 w = 0, h = 0;
  bool ok = read_back_texture(bb, px, w, h);
  bb->Release();
  if (!ok) return;
  for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // BGRA -> RGBA
  static u32 n = 0;
  std::string path = cfg().run_dir + ac4::strprintf("\\output_%04u.png", n++);
  std::thread([path, w, h, px = std::move(px)] {
    if (write_png(path, w, h, px)) LOG_I("gpu", "captured %s (%ux%u, the window's output)", path.c_str(), w, h);
  }).detach();
}

// The swap chain follows the window's client area (window resized, fullscreen toggled).
void resize_output() {
  RECT r{};
  if (!g_hwnd || !GetClientRect(g_hwnd, &r)) return;
  u32 w = u32(r.right - r.left), h = u32(r.bottom - r.top);
  if (!w || !h || (w == g_dev.width && h == g_dev.height)) return;  // minimized, or unchanged
  g_dev.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  if (g_dev.backbuffer_rtv) g_dev.backbuffer_rtv->Release();
  g_dev.backbuffer_rtv = nullptr;
  if (HRESULT hr = g_dev.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swap_flags); FAILED(hr)) {
    report_failure("swap chain resize", hr);
  } else {
    g_dev.width = w;
    g_dev.height = h;
    LOG_I("gpu", "output %ux%u", w, h);
  }
  ID3D11Texture2D* bb = nullptr;
  if (SUCCEEDED(g_dev.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
    g_dev.dev->CreateRenderTargetView(bb, nullptr, &g_dev.backbuffer_rtv);
    bb->Release();
  }
}

// Presentation statistics, logged every 5 s: for how many refreshes of the display each frame
// stayed on screen (DXGI frame statistics: the refresh each displayed present appeared on). With
// even pacing every frame stays equally long; a frame that stays longer than the pacing intends
// is a repeat the player sees. A sample may cover several presents: with sync interval k each
// frame stays at least k refreshes, so n presents over exactly n*k refreshes all stayed k; other
// multi-present samples are counted as unresolved. The statistics only advance while the system
// composes the window: a covered or minimized window (reported as foreground time and occluded
// presents) is not a pacing measurement.
struct PresentStats {
  bool have = false;
  UINT count = 0, refresh = 0;  // the last displayed present and the refresh it appeared on
  bool sync = false;
  UINT sync_refresh0 = 0, sync_refresh1 = 0;  // vblank samples (measured refresh rate)
  s64 sync_qpc0 = 0, sync_qpc1 = 0;
  u64 presents = 0, shown[6] = {}, multi = 0, multi_presents = 0, errors = 0, occluded = 0, foreground = 0;
  u32 intervals = 0;  // bit k: sync interval k used
  double start = -1;
};
PresentStats g_ps;

void present_statistics(u32 interval, HRESULT present_result) {
  PresentStats& p = g_ps;
  ++p.presents;
  p.occluded += present_result == DXGI_STATUS_OCCLUDED;
  p.foreground += GetForegroundWindow() == g_hwnd;
  p.intervals |= 1u << std::min<u32>(interval, 7);
  DXGI_FRAME_STATISTICS st{};
  HRESULT hr = g_dev.swap->GetFrameStatistics(&st);
  if (FAILED(hr)) {
    ++p.errors;
    p.have = false;
  } else {
    if (p.have && st.PresentCount != p.count) {
      UINT dp = st.PresentCount - p.count, dr = st.PresentRefreshCount - p.refresh;
      if (dp == 1) {
        ++p.shown[std::min<UINT>(dr, 5)];
      } else if (interval >= 1 && interval <= 5 && dr == dp * interval) {
        p.shown[interval] += dp;
      } else {
        ++p.multi;
        p.multi_presents += dp;
      }
    }
    if (!p.have || st.PresentCount != p.count) {
      p.count = st.PresentCount;
      p.refresh = st.PresentRefreshCount;
      p.have = true;
    }
    if (st.SyncRefreshCount && st.SyncQPCTime.QuadPart) {
      if (!p.sync) {
        p.sync = true;
        p.sync_refresh0 = st.SyncRefreshCount;
        p.sync_qpc0 = st.SyncQPCTime.QuadPart;
      }
      p.sync_refresh1 = st.SyncRefreshCount;
      p.sync_qpc1 = st.SyncQPCTime.QuadPart;
    }
  }
  double now = log_time();
  if (p.start < 0) p.start = now;
  if (now - p.start < 5.0) return;
  std::string ivs;
  for (u32 k = 0; k < 8; ++k)
    if (p.intervals & (1u << k)) ivs += ac4::strprintf("%s%u", ivs.empty() ? "" : "/", k);
  std::string measured = "-";
  if (p.sync && p.sync_qpc1 > p.sync_qpc0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    measured = ac4::strprintf("%.2f", double(p.sync_refresh1 - p.sync_refresh0) * double(f.QuadPart) / double(p.sync_qpc1 - p.sync_qpc0));
  }
  LOG_I("present",
        "%.1f s: %llu presents (sync interval %s); frames on screen for 1/2/3/4/5+ refreshes: %llu/%llu/%llu/%llu/%llu, same "
        "refresh %llu, unresolved %llu samples (%llu presents), statistics unavailable %llu; window in front %.0f%%, occluded "
        "presents %llu; display %.2f Hz, measured %s Hz",
        now - p.start, (unsigned long long)p.presents, ivs.c_str(), (unsigned long long)p.shown[1], (unsigned long long)p.shown[2],
        (unsigned long long)p.shown[3], (unsigned long long)p.shown[4], (unsigned long long)p.shown[5],
        (unsigned long long)p.shown[0], (unsigned long long)p.multi, (unsigned long long)p.multi_presents,
        (unsigned long long)p.errors, 100.0 * double(p.foreground) / double(p.presents), (unsigned long long)p.occluded,
        display_refresh_hz(), measured.c_str());
  bool have = p.have;
  UINT count = p.count, refresh = p.refresh;
  p = PresentStats{};
  p.have = have;
  p.count = count;
  p.refresh = refresh;
  p.start = now;
}

}  // namespace

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
  auto draw_to = [&](ID3D11RenderTargetView* rtv, float x, float y, float w, float h) {
    D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
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
  draw_to(last_rtv, 0, 0, float(src_w), float(src_h));
  last_rtv->Release();
  if (g_dev.swap) {
    resize_output();
    if (!g_dev.backbuffer_rtv) return;
    // The PS3's output is 16:9: scaled to fit the window, centred, the rest black.
    float bw = float(g_dev.width), bh = float(g_dev.height), vw = bw, vh = bh;
    if (bw * 9 > bh * 16) vw = std::round(bh * 16 / 9);
    else vh = std::round(bw * 9 / 16);
    if (vw < bw || vh < bh) {
      const FLOAT black[4] = {0, 0, 0, 1};
      c->ClearRenderTargetView(g_dev.backbuffer_rtv, black);
    }
    draw_to(g_dev.backbuffer_rtv, std::floor((bw - vw) / 2), std::floor((bh - vh) / 2), vw, vh);
    if (g_output_shots.load(std::memory_order_relaxed) > 0) {
      g_output_shots.fetch_sub(1);
      capture_output();
    }
    u32 k = g_interval.load(std::memory_order_relaxed);
    HRESULT hr = g_dev.swap->Present(k, k == 0 && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) report_failure("Present", hr);
    present_statistics(k, hr);
  }
}

bool read_back_last_frame(std::vector<u8>& rgba, u32& w, u32& h) { return read_back_texture(g_last, rgba, w, h); }

struct PendingCapture {
  ID3D11Texture2D* staging = nullptr;
  u32 index = 0, w = 0, h = 0;
};
std::vector<PendingCapture> g_captures;

void begin_capture(u32 index) {
  if (!g_last || g_captures.size() >= 8) return;
  D3D11_TEXTURE2D_DESC td{};
  g_last->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  PendingCapture c;
  if (HRESULT hr = g_dev.dev->CreateTexture2D(&td, nullptr, &c.staging); FAILED(hr)) {
    report_failure("capture staging texture", hr);
    return;
  }
  g_dev.ctx->CopyResource(c.staging, g_last);
  c.index = index;
  c.w = td.Width;
  c.h = td.Height;
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
