#include "runtime/gpu/display.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_5.h>

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "runtime/config.h"
#include "runtime/log.h"

namespace rt::lv2 {
u64 system_time_us();
void sleep_us(u64 us);
}  // namespace rt::lv2

namespace rt::gpu::display {
namespace {

std::mutex g_mu;
IDXGIOutput* g_output = nullptr;  // output containing the window (held reference), set by the presenting thread
std::wstring g_output_name;
u32 g_refresh = 0;  // Hz of the output's current display mode (integer, as Windows reports it)
u32 g_divisor = 0;  // display-locked pacing: an emulated vblank every N display vblanks (0: clock)
double g_div_fps = 0;  // the target g_divisor was chosen for (the target changes during movies)
bool g_tearing = false;
std::atomic<bool> g_exclusive{false};
std::atomic<bool> g_locked{false};  // the last vblank came from the display (else the clock)

// Video decoders open (movie playback): the system's movie player (libsail, as firmware code)
// stalls or starts late with the emulated vblank above 60 Hz (GAME_NOTES "Frame pacing": the
// opening movie at 120 Hz decoded 4 pictures and stopped; 75 Hz started 13 s late), and the
// movies are 30 fps: while one plays, the vblank runs at 60 Hz.
std::atomic<int> g_movies{0};
std::atomic<int> g_gameplay{0};  // missions running (frame_policy.cpp)

// The game's frame rate target: --fps N (default 60), the display's refresh (--fps display) or
// uncapped (a vblank every millisecond: the game's own work sets the rate). The game advances its
// simulation by measured time (GAME_NOTES "Frame pacing": movement, turning and per-frame motion
// identical at 60 and 120 Hz), so the emulated vblank rate sets the frame rate. Other targets than
// 60 apply in missions only: some menu fades count frames, and movies need 60.
constexpr double kUncappedHz = 1000;
double target_fps() {
  double want = cfg().fps, fps;
  if (want < 0) {
    fps = kUncappedHz;
  } else if (want > 0) {
    fps = want;
  } else {
    std::lock_guard lk(g_mu);
    fps = g_refresh >= 30 ? double(g_refresh) : 60.0;
  }
  bool outside = g_movies.load(std::memory_order_relaxed) > 0 || g_gameplay.load(std::memory_order_relaxed) == 0;
  return outside && std::fabs(fps - 60.0) > 0.5 ? 60.0 : fps;
}

// Display-locked pacing needs a refresh that is a whole multiple of the target (within 1.5 Hz).
u32 divisor_for(u32 hz, double fps) {
  if (hz < 30 || fps <= 0) return 0;
  u32 n = u32(std::lround(double(hz) / fps));
  return n && std::fabs(double(hz) / n - fps) <= 1.5 ? n : 0;
}
// The divisor under the configured pacing (auto, display, clock).
u32 pacing_divisor(u32 hz, double fps) {
  if (fps >= kUncappedHz) return 0;
  u32 div = divisor_for(hz, fps);
  const std::string& want = cfg().pacing;
  if (want == "clock") div = 0;
  else if (want == "display" && !div && hz) div = std::max<u32>(1, u32(std::lround(double(hz) / fps)));
  return div;
}

// ---- clock pacing (the target rate from the performance counter) ----
double g_next_tick_us = 0;
void wait_clock() {
  const double fps = target_fps();
  if (fps >= kUncappedHz) {  // uncapped: a tick every millisecond, slept (the exact rate is irrelevant)
    g_next_tick_us = 0;
    lv2::sleep_us(1000);
    return;
  }
  const double period = 1e6 / fps;
  double now = double(lv2::system_time_us());
  if (g_next_tick_us == 0 || now - g_next_tick_us > 4 * period) g_next_tick_us = now;  // start or resynchronise
  g_next_tick_us += period;
  for (;;) {
    now = double(lv2::system_time_us());
    double left = g_next_tick_us - now;
    if (left <= 0) return;
    if (left > 2000) lv2::sleep_us(u64(left - 1000));
    else _mm_pause();
  }
}

// ---- presentation statistics ----
struct Stats {
  u64 displayed = 0, unresolved = 0, presents = 0;
  u64 by_refreshes[5] = {};  // frames shown for 1, 2, 3, 4, 5+ refreshes (index n-1)
  std::vector<float> interval_ms;
  u64 unavailable = 0;
};
Stats g_stats;  // written by the presenting thread, read by report() (perf thread)
std::mutex g_stats_mu;
UINT g_last_count = 0, g_last_refresh = 0;
LARGE_INTEGER g_last_qpc{};
bool g_have_last = false;
double g_qpc_ms = 0;
u64 g_presents_since_output_check = 0;

void update_output(IDXGISwapChain1* swap) {
  IDXGIOutput* out = nullptr;
  if (FAILED(swap->GetContainingOutput(&out)) || !out) return;
  DXGI_OUTPUT_DESC od{};
  out->GetDesc(&od);
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  u32 hz = EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  {
    std::lock_guard lk(g_mu);
    if (g_output && g_output_name == od.DeviceName && g_refresh == hz) {
      out->Release();
      return;
    }
    if (g_output) g_output->Release();
    g_output = out;
    g_output_name = od.DeviceName;
    g_refresh = hz;
  }
  double fps = target_fps();
  u32 div = pacing_divisor(hz, fps);
  {
    std::lock_guard lk(g_mu);
    g_divisor = div;
    g_div_fps = fps;
  }
  char name[64];
  WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, name, sizeof name, nullptr, nullptr);
  if (div)
    LOG_I("display", "output %s at %u Hz, target %.0f fps: the game's vblank follows every %u%s display vblank (%.2f Hz)", name, hz,
          fps, div, div == 1 ? "" : div == 2 ? "nd" : div == 3 ? "rd" : "th", double(hz) / div);
  else
    LOG_I("display", "output %s at %u Hz, target %.0f fps: not a whole multiple; the game's vblank runs from a %.0f Hz clock%s", name,
          hz, fps, fps, g_tearing ? ", frames presented with tearing allowed (even pacing on variable-refresh displays)" : "");
}

}  // namespace

void movie_started() {
  if (g_movies.fetch_add(1) == 0 && cfg().fps != 60)
    LOG_I("display", "video playback: the emulated vblank runs at up to 60 Hz until it ends");
}
void movie_stopped() {
  if (g_movies.fetch_sub(1) == 1 && cfg().fps != 60) LOG_I("display", "video playback ended");
}
void gameplay_started() {
  if (g_gameplay.fetch_add(1) == 0 && cfg().fps != 60)
    LOG_I("display", "mission started: frame rate target %s", cfg().fps < 0 ? "uncapped" : ac4::strprintf("%.0f", target_fps()).c_str());
}
void gameplay_stopped() {
  if (g_gameplay.fetch_sub(1) == 1 && cfg().fps != 60) LOG_I("display", "mission ended: 60 Hz outside missions");
}

void wait_next_vblank() {
  IDXGIOutput* out = nullptr;
  u32 div = 0;
  double fps = target_fps();  // before the lock (it may take it)
  {
    std::lock_guard lk(g_mu);
    if (g_output && fps != g_div_fps) {  // the target changed (movie playback): pacing for the new one now
      g_divisor = pacing_divisor(g_refresh, fps);
      g_div_fps = fps;
    }
    if (g_output && g_divisor) {
      out = g_output;
      out->AddRef();
      div = g_divisor;
    }
  }
  if (out) {
    bool ok = true;
    for (u32 i = 0; i < div && ok; ++i) ok = SUCCEEDED(out->WaitForVBlank());
    out->Release();
    if (ok) {
      g_locked = true;
      g_next_tick_us = 0;
      return;
    }
  }
  g_locked = false;
  wait_clock();
}

u32 present_flags() {
  std::lock_guard lk(g_mu);
  return g_tearing && !g_divisor && !g_exclusive.load() ? DXGI_PRESENT_ALLOW_TEARING : 0;
}

void set_exclusive(bool on) { g_exclusive = on; }

u32 refresh_hz() {
  std::lock_guard lk(g_mu);
  return g_refresh;
}

std::vector<Mode> modes() {
  std::vector<Mode> out;
  std::lock_guard lk(g_mu);
  if (!g_output) return out;
  UINT n = 0;
  if (FAILED(g_output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &n, nullptr)) || !n) return out;
  std::vector<DXGI_MODE_DESC> list(n);
  if (FAILED(g_output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &n, list.data()))) return out;
  for (UINT i = 0; i < n; ++i) {
    const DXGI_MODE_DESC& d = list[i];
    if (d.Width < 640 || d.Height < 360 || !d.RefreshRate.Denominator) continue;
    u32 hz = u32(std::lround(double(d.RefreshRate.Numerator) / d.RefreshRate.Denominator));
    bool seen = std::any_of(out.begin(), out.end(), [&](const Mode& m) { return m.w == d.Width && m.h == d.Height && m.hz == hz; });
    if (!seen && hz >= 30) out.push_back({d.Width, d.Height, hz});
  }
  std::sort(out.begin(), out.end(), [](const Mode& a, const Mode& b) {
    return a.w != b.w ? a.w > b.w : a.h != b.h ? a.h > b.h : a.hz > b.hz;
  });
  return out;
}

void swapchain_created(IDXGISwapChain1* swap, bool tearing) {
  {
    std::lock_guard lk(g_mu);
    g_tearing = tearing;
  }
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpc_ms = 1000.0 / double(f.QuadPart);
  update_output(swap);
}

void after_present(IDXGISwapChain1* swap) {
  std::lock_guard slk(g_stats_mu);
  ++g_stats.presents;
  if (++g_presents_since_output_check >= 120) {  // the window may have moved to another output
    g_presents_since_output_check = 0;
    update_output(swap);
  }
  DXGI_FRAME_STATISTICS st{};
  HRESULT hr = swap->GetFrameStatistics(&st);
  if (FAILED(hr)) {
    ++g_stats.unavailable;
    g_have_last = false;
    return;
  }
  if (!st.PresentCount) return;
  if (g_have_last && st.PresentCount != g_last_count) {
    UINT np = st.PresentCount - g_last_count, nr = st.PresentRefreshCount - g_last_refresh;
    if (np == 1) {
      ++g_stats.displayed;
      if (nr >= 1) ++g_stats.by_refreshes[std::min<UINT>(nr, 5) - 1];
      // Display time of this frame is known when it was displayed at the latest vblank.
      if (st.SyncRefreshCount == st.PresentRefreshCount && g_last_qpc.QuadPart)
        g_stats.interval_ms.push_back(float(double(st.SyncQPCTime.QuadPart - g_last_qpc.QuadPart) * g_qpc_ms));
    } else {
      g_stats.unresolved += np;
    }
  }
  g_last_qpc = st.SyncRefreshCount == st.PresentRefreshCount ? st.SyncQPCTime : LARGE_INTEGER{};
  g_last_count = st.PresentCount;
  g_last_refresh = st.PresentRefreshCount;
  g_have_last = true;
}

std::string report() {
  Stats s;
  {
    std::lock_guard slk(g_stats_mu);
    s = std::move(g_stats);
    g_stats = Stats{};
  }
  u32 hz, div;
  bool tearing;
  {
    std::lock_guard lk(g_mu);
    hz = g_refresh;
    div = g_divisor;
    tearing = g_tearing;
  }
  std::string pacing = div && g_locked.load() ? ac4::strprintf("display-locked every %u vblank%s", div, div > 1 ? "s" : "")
                           : target_fps() >= kUncappedHz ? std::string(tearing ? "uncapped, tearing allowed" : "uncapped")
                                                         : ac4::strprintf("%.0f Hz clock%s", target_fps(), tearing ? ", tearing allowed" : "");
  if (!s.presents) return ac4::strprintf("display: %u Hz, %s; no frames presented", hz, pacing.c_str());
  std::string shown;
  u64 known = 0;
  for (u64 n : s.by_refreshes) known += n;
  if (known)
    for (int i = 0; i < 5; ++i)
      if (s.by_refreshes[i])
        shown += ac4::strprintf("%s%d%s: %.1f%%", shown.empty() ? "" : ", ", i + 1, i == 4 ? "+" : "", 100.0 * double(s.by_refreshes[i]) / double(known));
  std::string iv;
  if (!s.interval_ms.empty()) {
    std::sort(s.interval_ms.begin(), s.interval_ms.end());
    auto pct = [&](double p) { return s.interval_ms[std::min(s.interval_ms.size() - 1, size_t(p * double(s.interval_ms.size())))]; };
    iv = ac4::strprintf("; displayed-frame intervals (%zu): median %.2f ms, p1 %.2f, p99 %.2f, max %.2f", s.interval_ms.size(), pct(0.5),
                         pct(0.01), pct(0.99), s.interval_ms.back());
  }
  return ac4::strprintf("display: %u Hz, %s; %llu presented, %llu displayed (%llu unresolved%s); refreshes per frame %s%s", hz,
                         pacing.c_str(), (unsigned long long)s.presents, (unsigned long long)s.displayed,
                         (unsigned long long)s.unresolved,
                         s.unavailable ? ac4::strprintf(", statistics unavailable %llu", (unsigned long long)s.unavailable).c_str() : "",
                         shown.empty() ? "-" : shown.c_str(), iv.c_str());
}

}  // namespace rt::gpu::display
