// RSX command processor: FIFO fetch (jump/call/return), method register file, sync methods
// (reference, semaphores, labels, reports), memory transfer engines, flips and vblank.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/lv2/lv2.h"
#include "runtime/memory.h"
#include "runtime/ppu_thread.h"
#include "runtime/rsx/methods.h"
#include "runtime/rsx/rsx.h"
#include "runtime/profiler.h"
#include "runtime/rsx/rsx_internal.h"

namespace rt::lv2 {
void sleep_us(u64 us);
}

namespace rt::rsx {

State g;
std::atomic<u64> g_fifo_idle_us{0};

namespace {

std::mutex g_io_mu;
u32 g_io_ea[512];   // io >> 20 -> ea >> 20 (0xffffffff = unmapped)
u32 g_ea_io[4096];  // ea >> 20 -> io >> 20
bool g_init = false;
u32 g_local_size = 0;

// ---- callbacks into guest code (vblank / flip handlers) ----
class CallbackQueue {
 public:
  void start() {
    thread_create_host("_gcm_intr_thread", 1, 0x10000, [this](ppu::Context& ctx) { loop(ctx); });
  }
  void post(u32 opd, u64 arg) {
    {
      std::lock_guard lk(m_);
      q_.push_back({opd, arg});
    }
    cv_.notify_one();
  }

 private:
  void loop(ppu::Context& ctx) {
    for (;;) {
      std::pair<u32, u64> job;
      {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return !q_.empty(); });
        job = q_.front();
        q_.pop_front();
      }
      hle::call_guest(ctx, job.first, {job.second});
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::pair<u32, u64>> q_;
};
CallbackQueue g_intr;

std::atomic<u32> g_flip_status{0};
std::atomic<u64> g_last_flip_time{0};
std::atomic<u32> g_flip_mode{1};  // CELL_GCM_DISPLAY_VSYNC
std::atomic<u32> g_flip_handler{0}, g_vblank_handler{0};
std::atomic<u64> g_vblank{0};
u64 g_flip_vblank = ~0ull;  // vblank period of the last flip (FIFO thread only)
std::mutex g_vb_mu;
std::condition_variable g_vb_cv;
std::mutex g_db_mu;
DisplayBuffer g_display[8];

void vblank_thread() {
  SetThreadDescription(GetCurrentThread(), L"rsx-vblank");
  LARGE_INTEGER f, t0, now;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  const double period = 1001.0 / 60000.0;  // 59.94 Hz
  u64 n = 0;
  for (;;) {
    ++n;
    for (;;) {
      QueryPerformanceCounter(&now);
      double el = double(now.QuadPart - t0.QuadPart) / double(f.QuadPart);
      double left = n * period - el;
      if (left <= 0) break;
      if (left > 0.002) lv2::sleep_us(u64((left - 0.001) * 1e6));
      else _mm_pause();
    }
    {
      std::lock_guard lk(g_vb_mu);
      g_vblank.fetch_add(1);
    }
    g_vb_cv.notify_all();
    if (u32 h = g_vblank_handler.load()) g_intr.post(h, 1);
  }
}

}  // namespace

namespace {
// A guest thread waiting for the FIFO to leave a command buffer segment (wait_get_outside).
std::atomic<bool> g_get_waiting{false};
std::atomic<u32> g_get_lo{0}, g_get_hi{0};
HANDLE g_get_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

inline void notify_get(u32 get) {
  if (!g_get_waiting.load(std::memory_order_relaxed)) return;
  if (get >= g_get_lo.load(std::memory_order_relaxed) && get <= g_get_hi.load(std::memory_order_relaxed)) return;
  g_get_waiting.store(false, std::memory_order_relaxed);
  SetEvent(g_get_event);
}
}  // namespace

// --fps-limit N: flips at most N times per second, on an even schedule (a late frame does not
// make the next ones early: the schedule restarts after a miss). Coarse waits sleep on a
// high-resolution timer; the last fraction of a millisecond spins for precision.
std::atomic<s64> g_movie_frame_qpc{0};
void note_movie_frame() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  g_movie_frame_qpc.store(now.QuadPart, std::memory_order_relaxed);
}
bool movie_playing() {
  static LARGE_INTEGER freq{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  s64 last = g_movie_frame_qpc.load(std::memory_order_relaxed);
  return last && now.QuadPart - last < freq.QuadPart / 4;  // a frame fetched in the last 250 ms
}

void pace_flip(double rate) {
  static LARGE_INTEGER freq{}, next{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  const s64 period = s64(double(freq.QuadPart) / rate);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!next.QuadPart || now.QuadPart - next.QuadPart > period) next.QuadPart = now.QuadPart;  // first flip or missed
  for (;;) {
    QueryPerformanceCounter(&now);
    s64 left = next.QuadPart - now.QuadPart;
    if (left <= 0) break;
    double us = double(left) * 1e6 / double(freq.QuadPart);
    if (us > 1500) lv2::sleep_us(u64(us - 1000));
    else _mm_pause();
  }
  next.QuadPart += period;
}

void wait_get_outside(u32 lo, u32 hi) {
  auto* ctrl_get = reinterpret_cast<volatile u32*>(ppu::mem() + kControlAddr + 4);
  auto outside = [&] {
    u32 get = __builtin_bswap32(*ctrl_get);
    return get < lo || get > hi;
  };
  if (outside()) return;
  note_block();
  g_get_lo = lo;
  g_get_hi = hi;
  for (;;) {
    g_get_waiting.store(true, std::memory_order_seq_cst);
    if (outside()) break;
    WaitForSingleObject(g_get_event, 2);  // the timeout only guards against a lost wake-up
  }
  g_get_waiting.store(false, std::memory_order_relaxed);
}

void clear_flip_label() {
  ppu::w64(kLabelAddr + 0x10, 0);
  ppu::w64(kLabelAddr + 0x18, 0);
}

// ---------------------------------------------------------------------------------------------
// Register reset state (rsx_facts.md ??3-7: the documented defaults). Games rely on registers they
// never write, e.g. the front face (CCW) when they enable culling.
void init_register_defaults() {
  auto set = [](u32 method, u32 v) { g.regs[method >> 2] = v; };
  auto setf = [&](u32 method, float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    set(method, v);
  };
  set(0x08E4, 1);           // SHADER_PROGRAM: local, offset 0
  set(0x1FF0, 0xFFFF);      // VERTEX_ATTRIB_INPUT_MASK
  set(0x1FF4, 0x3FFFFF);    // VERTEX_ATTRIB_OUTPUT_MASK
  set(0x1D88, 0x1000);      // SHADER_WINDOW
  set(0x1D64, 0x2000000);   // INDEXED_CONSTANT_READ_LIMITS
  for (u32 i = 0; i < 16; ++i) {
    set(0x1740 + 4 * i, 0x2);         // VERTEX_DATA_ARRAY_FORMAT: float, size 0 (disabled)
    set(0x0B00 + 4 * i, 0x2DC8);      // TEXTURE_CONTROL2
    set(0x03C0 + 4 * i, 0x10101);     // TEXTURE_ANISO_SPREAD
    set(0x1A08 + 0x20 * i, 0x30101);  // TEXTURE_ADDRESS: wrap, wrap, clamp to edge
    set(0x1A0C + 0x20 * i, 0x60000);  // TEXTURE_CONTROL0: lod 0..12, disabled
    set(0x1A14 + 0x20 * i, 0x2052000);  // TEXTURE_FILTER
  }
  set(0x1DB0, 0xFFFFFFFF);  // RESTART_INDEX
  set(0x02C0, 0x0FFF0000);  // WINDOW_CLIP_HORIZONTAL
  set(0x02C4, 0x0FFF0000);  // WINDOW_CLIP_VERTICAL
  set(0x03B0, 0x100000);    // CONTROL0
  set(0x1D80, 3);           // SURFACE_COMPRESSION
  set(0x0208, 0x121);       // SURFACE_FORMAT
  set(0x0320, 0x80068006);  // BLEND_EQUATION: add, add
  set(0x0314, 0x00010001);  // BLEND_FUNC_SFACTOR: one, one
  set(0x0318, 0);           // BLEND_FUNC_DFACTOR: zero, zero
  set(0x0324, 0x01010101);  // COLOR_MASK
  set(0x0A6C, 0x201);       // DEPTH_FUNC: less
  set(0x0A70, 1);           // DEPTH_MASK
  set(0x0330, 0x207);       // STENCIL_FUNC: always (front, back)
  set(0x0350, 0x207);
  for (u32 m : {0x032Cu, 0x034Cu, 0x0338u, 0x0358u}) set(m, 0xFF);  // stencil write / func masks
  for (u32 m : {0x033Cu, 0x0340u, 0x0344u, 0x035Cu, 0x0360u, 0x0364u}) set(m, 0x1E00);  // stencil ops: keep
  setf(0x0388, 1.0f);       // DEPTH_BOUNDS_MAX
  set(0x1830, 0x0405);      // CULL_FACE: back
  set(0x1834, 0x0901);      // FRONT_FACE: counter-clockwise
  set(0x1828, 0x1B02);      // FRONT_POLYGON_MODE: fill
  set(0x182C, 0x1B02);      // BACK_POLYGON_MODE: fill
  set(0x0368, 0x1D01);      // SHADE_MODE: smooth
  set(0x03B8, 8);           // LINE_WIDTH: 1.0
  set(0x0378, 0x1503);      // LOGIC_OP: copy
  set(0x0308, 0x207);       // ALPHA_FUNC: always
  for (u32 c = 0; c < 3; ++c) {
    setf(0x0A20 + 4 * c, c < 2 ? 2048.0f : 0.5f);  // VIEWPORT_OFFSET
    setf(0x0A30 + 4 * c, c < 2 ? 2048.0f : 0.5f);  // VIEWPORT_SCALE
  }
  setf(0x0398, 1.0f);       // CLIP_MAX
  set(0x1D78, 1);           // ZMIN_MAX_CONTROL: depth clip
  set(0x0A00, 0x10000000);  // VIEWPORT_HORIZONTAL / VERTICAL: 0..4096
  set(0x0A04, 0x10000000);
  set(0x08C0, 0x10000000);  // SCISSOR_HORIZONTAL / VERTICAL: 0..4096
  set(0x08C4, 0x10000000);
  set(0x1D8C, 0xFFFFFF00);  // ZSTENCIL_CLEAR_VALUE
  set(0x1D7C, 0xFFFF0000);  // ANTI_ALIASING_CONTROL
  set(0x08CC, 0x0800);      // FOG_MODE: exp
  set(0x1E98, 0x01000000);  // RENDER_ENABLE: always
  set(0x0300, 1);           // DITHER_ENABLE
  set(0x1D84, 3);           // ZCULL_EN
}

void init(u32 io_ea, u32 io_size, u32 local_size) {
  init_register_defaults();
  std::lock_guard lk(g_io_mu);
  std::fill(std::begin(g_io_ea), std::end(g_io_ea), 0xffffffffu);
  std::fill(std::begin(g_ea_io), std::end(g_ea_io), 0xffffffffu);
  g_local_size = local_size;
  if (!vm::alloc_fixed(vm::Area::Video, kLocalBase, local_size)) fatal("cannot map RSX local memory");
  if (!vm::alloc_fixed(vm::Area::Mmapper, kDeviceBase, kDeviceSize)) fatal("cannot map RSX device area");
  // Reports: semaphores get recognisable fill patterns (LV2 does this), timestamps -1.
  for (u32 i = 0; i < 1024; i += 4) {
    ppu::w32(kReportsBase + 4 * i + 0, 0x1337C0D3);
    ppu::w32(kReportsBase + 4 * i + 4, 0x1337BABE);
    ppu::w32(kReportsBase + 4 * i + 8, 0x1337BEEF);
    ppu::w32(kReportsBase + 4 * i + 12, 0x1337F001);
  }
  // Label 1 is the flip semaphore libgcm's wait-for-flip polls (acquire == 0). It reads 0 before
  // the first flip: this game's first frame waits on it before queueing any flip. (The fill
  // pattern above is what the reference emulator writes; it is not evidence for label 1.)
  clear_flip_label();
  for (u32 i = 0; i < 64; ++i) ppu::w64(kNotifyAddr + 16 * i, ~0ull);
  for (u32 i = 0; i < 2048; ++i) {
    ppu::w64(kReportAddr + 16 * i, ~0ull);
    ppu::w32(kReportAddr + 16 * i + 8, 0);
    ppu::w32(kReportAddr + 16 * i + 12, ~0u);
  }
  g_init = true;
  (void)io_ea;
  (void)io_size;
}

bool initialized() { return g_init; }
u32 local_size() { return g_local_size; }

bool map_io(u32 ea, u32 io, u32 size) {
  if ((ea & 0xfffff) || (io & 0xfffff) || (size & 0xfffff) || !size) return false;
  std::lock_guard lk(g_io_mu);
  for (u32 i = 0; i < size >> 20; ++i) {
    u32 ioi = (io >> 20) + i, eai = (ea >> 20) + i;
    if (ioi >= 512 || g_io_ea[ioi] != 0xffffffffu || g_ea_io[eai] != 0xffffffffu) return false;
  }
  for (u32 i = 0; i < size >> 20; ++i) {
    g_io_ea[(io >> 20) + i] = (ea >> 20) + i;
    g_ea_io[(ea >> 20) + i] = (io >> 20) + i;
  }
  LOG_I("rsx", "io map ea 0x%08x -> io 0x%08x (0x%x bytes)", ea, io, size);
  return true;
}

bool unmap_io(u32 io, u32 size) {
  std::lock_guard lk(g_io_mu);
  for (u32 i = 0; i < size >> 20; ++i) {
    u32 ioi = (io >> 20) + i;
    if (ioi >= 512) return false;
    if (g_io_ea[ioi] != 0xffffffffu) g_ea_io[g_io_ea[ioi]] = 0xffffffffu;
    g_io_ea[ioi] = 0xffffffffu;
  }
  return true;
}

bool io_to_ea(u32 io, u32* ea) {
  u32 i = io >> 20;
  if (i >= 512) return false;
  u32 e = g_io_ea[i];
  if (e == 0xffffffffu) return false;
  *ea = (e << 20) | (io & 0xfffff);
  return true;
}

bool ea_to_io(u32 ea, u32* io) {
  u32 i = g_ea_io[ea >> 20];
  if (i == 0xffffffffu) return false;
  *io = (i << 20) | (ea & 0xfffff);
  return true;
}

u32 offset_to_ea(u32 offset, u32 location) {
  if (location == 0) return offset < g_local_size ? kLocalBase + offset : 0;
  u32 ea;
  return io_to_ea(offset, &ea) ? ea : 0;
}

void set_display_buffer(u32 id, const DisplayBuffer& b) {
  std::lock_guard lk(g_db_mu);
  if (id < 8) g_display[id] = b;
}
u32 flip_status() { return g_flip_status.load(); }
void reset_flip_status() { g_flip_status = 1; }
u64 last_flip_time() { return g_last_flip_time.load(); }
void set_flip_mode(u32 mode) { g_flip_mode = mode; }
void set_flip_handler(u32 opd) { g_flip_handler = opd; }
void set_vblank_handler(u32 opd) { g_vblank_handler = opd; }
u64 vblank_count() { return g_vblank.load(); }

// ---------------------------------------------------------------------------------------------
// Flip: wait for vblank (vsync mode), hand the buffer to the presenter, update status, notify.
// In hsync mode the hardware switches buffers at the next scanline; the display still shows at
// most one new frame per refresh, and the RSX's rendering time kept the game far below that rate.
// Without a model of rendering time, a second flip within the same refresh period waits for the
// next vblank (otherwise the game would run its loop several times per displayed frame).
void do_flip(u32 id) {
  if (double ms = cfg().present_delay_ms; ms > 0) {  // diagnostics: a slow GPU, i.e. a low frame rate
    if (static bool logged = false; !logged) {
      logged = true;
      LOG_W("rsx", "--present-delay: every flip waits %.1f ms more (diagnostics, not for play)", ms);
    }
    lv2::sleep_us(u64(ms * 1000));
  }
  double limit = cfg().fps_limit;
  if (limit >= 0 && movie_playing()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      LOG_I("rsx", "frame limit: movies keep the console's 59.94 Hz flip cadence (their frames are picked by flip time)");
    }
    limit = -1;
  }
  if (limit < 0) {
    // The console's pacing: at most one flip per vertical blank of the emulated 59.94 Hz display.
    if (g_flip_mode.load() == 1 || g_flip_vblank == g_vblank.load()) {
      std::unique_lock lk(g_vb_mu);
      u64 v = g_vblank.load();
      g_vb_cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return g_vblank.load() != v; });
    }
  } else if (limit > 0) {
    pace_flip(limit);
  }
  g_flip_vblank = g_vblank.load();
  DisplayBuffer b;
  {
    std::lock_guard lk(g_db_mu);
    b = g_display[id & 7];
  }
  present_buffer(id & 7, b);
  bump(g.flips);
  g_last_flip_time = lv2::system_time_us();
  g_flip_status = 0;  // CELL_GCM_DISPLAY_FLIP_STATUS_DONE
  clear_flip_label();  // hardware resets the 16 bytes of label 1 when a flip completes
  if (u32 h = g_flip_handler.load()) g_intr.post(h, 1);
}

// ---------------------------------------------------------------------------------------------
// FIFO
namespace {

u32 fifo_read(u32 io) {
  u32 ea;
  if (!io_to_ea(io, &ea)) fatal(ac4::strprintf("RSX FIFO fetch from unmapped io offset 0x%08x", io));
  return ppu::r32(ea);
}

void fifo_thread() {
  SetThreadDescription(GetCurrentThread(), L"rsx-fifo");
  prof::register_thread("rsx-fifo", OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                                 GetCurrentThreadId()));
  renderer_thread_init();
  auto* ctrl_put = reinterpret_cast<volatile u32*>(ppu::mem() + kControlAddr);
  auto* ctrl_get = reinterpret_cast<volatile u32*>(ppu::mem() + kControlAddr + 4);
  u32 get = __builtin_bswap32(*ctrl_get);
  std::vector<u32> call_stack;
  u32 idle = 0;
  u64 idle_since = 0;
  for (;;) {
    u32 put = __builtin_bswap32(*ctrl_put);
    if (get == g_covered_put) renderer_guest_sync();
    if (get == put) {
      if (idle == 0) idle_since = lv2::system_time_us();
      if (++idle < 64) {
        _mm_pause();
      } else {
        renderer_idle();
        lv2::sleep_us(idle < 2000 ? 20 : 200);
      }
      continue;
    }
    if (idle) g_fifo_idle_us.fetch_add(lv2::system_time_us() - idle_since, std::memory_order_relaxed);
    idle = 0;
    u32 cmd = fifo_read(get);
    if ((cmd & 0xe0000003) == 0x20000000) {  // old jump
      get = cmd & 0x1ffffffc;
    } else if ((cmd & 3) == 1) {  // new jump
      get = cmd & 0xfffffffc;
    } else if ((cmd & 3) == 2) {  // call
      if (call_stack.size() > 16) fatal("RSX FIFO call stack overflow");
      call_stack.push_back(get + 4);
      get = cmd & 0xfffffffc;
    } else if ((cmd & 0xffff0003) == 0x00020000) {  // return
      if (call_stack.empty()) fatal(ac4::strprintf("RSX FIFO return with empty call stack at 0x%08x", get));
      get = call_stack.back();
      call_stack.pop_back();
    } else if (cmd == 0) {
      get += 4;
    } else {
      u32 count = (cmd >> 18) & 0x7ff;
      u32 method = cmd & 0xfffc;
      bool noinc = (cmd & 0xe0030003) == 0x40000000;
      bool inc = (cmd & 0xe0030003) == 0;
      if (!inc && !noinc) {
        LOG_E("rsx", "unknown FIFO command 0x%08x at io 0x%08x; skipping", cmd, get);
        get += 4;
      } else {
        u32 args_io = get + 4;
        get += 4 + 4 * count;
        for (u32 i = 0; i < count; ++i) {
          u32 m = noinc ? method : method + 4 * i;
          execute_method(m >> 2, fifo_read(args_io + 4 * i), count, i);
        }
        bump(g.commands);
      }
    }
    *ctrl_get = __builtin_bswap32(get);
    notify_get(get);
  }
}

}  // namespace

void start() {
  auto* ctrl = reinterpret_cast<u32*>(ppu::mem() + kControlAddr);
  (void)ctrl;
  g_intr.start();
  std::thread(fifo_thread).detach();
  std::thread(vblank_thread).detach();
  LOG_I("rsx", "RSX threads started");
}

extern std::atomic<u32> g_acquire_ea, g_acquire_value, g_method_pos;
extern u32 g_method_ring[32][2];

void dump() {
  u32 put = ppu::r32(kControlAddr), get = ppu::r32(kControlAddr + 4), ref = ppu::r32(kControlAddr + 8);
  LOG_I("dump", "RSX: put 0x%08x get 0x%08x ref 0x%08x, %llu commands", put, get, ref, (unsigned long long)g.commands);
  if (u32 a = g_acquire_ea.load())
    LOG_I("dump", "RSX: waiting in semaphore acquire at 0x%08x for 0x%x (has 0x%x)", a, g_acquire_value.load(), ppu::r32(a));
  std::string last;
  u32 pos = g_method_pos.load();
  for (u32 i = 0; i < 32 && i < pos; ++i) {
    u32 k = (pos - 32 + i) & 31;
    if (pos < 32) k = i;
    last += ac4::strprintf(" %04x=%08x", g_method_ring[k][0], g_method_ring[k][1]);
  }
  LOG_I("dump", "RSX: last methods:%s", last.c_str());
}

u64 flips() { return g.flips; }

std::string stats() {
  return ac4::strprintf("rsx: %llu commands, %llu flips, %llu draws, %llu transfers, vblank %llu", (unsigned long long)g.commands,
                         (unsigned long long)g.flips, (unsigned long long)g.draws, (unsigned long long)g.transfers,
                         (unsigned long long)g_vblank.load());
}

}  // namespace rt::rsx
