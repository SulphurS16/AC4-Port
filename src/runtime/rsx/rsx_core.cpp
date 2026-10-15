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
#include "runtime/gpu/display.h"
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

std::atomic<u32> g_get_waiters{0}, g_get_wait_lo{0}, g_get_wait_hi{0};  // wait_get_outside()
// The FIFO's position in the command ring: GET, or while inside called command lists the
// outermost return address (the place in the ring it will continue from).
std::atomic<u32> g_ring_get{0};
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

// The emulated vertical blank: from the display's own vblank when its refresh is a multiple of
// about 60 Hz, else from a 60 Hz clock (gpu/display.h).
void vblank_thread() {
  SetThreadDescription(GetCurrentThread(), L"rsx-vblank");
  for (;;) {
    gpu::display::wait_next_vblank();
    {
      std::lock_guard lk(g_vb_mu);
      g_vblank.fetch_add(1);
    }
    g_vb_cv.notify_all();
    if (u32 h = g_vblank_handler.load()) g_intr.post(h, 1);
  }
}

}  // namespace

void clear_flip_label() {
  ppu::w64(kLabelAddr + 0x10, 0);
  ppu::w64(kLabelAddr + 0x18, 0);
}

// ---------------------------------------------------------------------------------------------
// Register reset state (rsx_facts.md §3-7: the documented defaults). Games rely on registers they
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
  if (g_flip_mode.load() == 1 || g_flip_vblank == g_vblank.load()) {
    std::unique_lock lk(g_vb_mu);
    u64 v = g_vblank.load();
    g_vb_cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return g_vblank.load() != v; });
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

// Recent FIFO control flow (jumps, calls, returns, new PUT values) for the report on the first
// invalid command: which path led the FIFO into data that is not a command.
struct FlowEvent {
  char kind;  // 'J' jump, 'C' call, 'R' return, 'P' new put
  u32 at, to;
  u64 t_us;
};
FlowEvent g_flow[64];
u32 g_flow_n = 0;
u32 g_ring_lo = 0, g_ring_hi = 0;  // the default command buffer's fragments (io offsets)
void flow(char kind, u32 at, u32 to) {
  g_flow[g_flow_n++ & 63] = {kind, at, to, lv2::system_time_us()};
}

void flow_report(const char* why, u32 get, u32 put) {
  static bool once = false;
  if (once) return;
  once = true;
  u32 ctx = kContextAddr;
  LOG_E("rsx", "FIFO report (%s): get 0x%08x put 0x%08x; libgcm context begin 0x%08x end 0x%08x current 0x%08x", why, get, put,
        ppu::r32(ctx), ppu::r32(ctx + 4), ppu::r32(ctx + 8));
  u64 now = lv2::system_time_us();
  for (u32 i = g_flow_n > 64 ? g_flow_n - 64 : 0; i < g_flow_n; ++i) {
    const FlowEvent& e = g_flow[i & 63];
    LOG_E("rsx", "  %c at 0x%08x -> 0x%08x (%.3f ms ago)", e.kind, e.at, e.to, double(now - e.t_us) / 1000.0);
  }
}

}  // namespace

void fifo_desync(const char* why, u32 value) {
  u32 n = g_method_pos.load(std::memory_order_relaxed);
  for (u32 i = n > 32 ? n - 32 : 0; i < n; ++i)
    LOG_E("rsx", "  method 0x%04x = 0x%08x", g_method_ring[i & 31][0], g_method_ring[i & 31][1]);
  std::string what = ac4::strprintf("%s (0x%08x)", why, value);
  flow_report(what.c_str(), __builtin_bswap32(*reinterpret_cast<volatile u32*>(ppu::mem() + kControlAddr + 4)), ppu::r32(kControlAddr));
  fatal("RSX FIFO desynchronized: " + what);
}

namespace {

u32 fifo_read(u32 io) {
  u32 ea;
  if (!io_to_ea(io, &ea)) {
    flow_report("fetch from an unmapped io offset", io, ppu::r32(kControlAddr));
    fatal(ac4::strprintf("RSX FIFO fetch from unmapped io offset 0x%08x", io));
  }
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
  g_ring_get.store(get);
  std::vector<u32> call_stack;
  const bool trace_methods = cfg().trace_rsx != 0;  // --trace-rsx logs every method: no bulk paths
  init_method_kinds();
  u32 idle = 0, last_put = ~0u;
  u64 idle_since = 0;
  for (;;) {
    u32 put = __builtin_bswap32(*ctrl_put);
    if (put != last_put) {
      flow('P', get, put);
      last_put = put;
    }
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
      flow('J', get, cmd & 0x1ffffffc);
      get = cmd & 0x1ffffffc;
    } else if ((cmd & 3) == 1) {  // new jump
      flow('J', get, cmd & 0xfffffffc);
      get = cmd & 0xfffffffc;
    } else if ((cmd & 3) == 2) {  // call
      if (call_stack.size() > 16) fatal("RSX FIFO call stack overflow");
      flow('C', get, cmd & 0xfffffffc);
      call_stack.push_back(get + 4);
      get = cmd & 0xfffffffc;
    } else if ((cmd & 0xffff0003) == 0x00020000) {  // return
      if (call_stack.empty()) fatal(ac4::strprintf("RSX FIFO return with empty call stack at 0x%08x", get));
      flow('R', get, call_stack.back());
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
        flow_report("invalid command", get, put);
        get += 4;
      } else {
        u32 args_io = get + 4;
        get += 4 + 4 * count;
        // The arguments through one host pointer when they lie in one io page (nearly always).
        u32 args_ea;
        const u8* args = count && (args_io >> 20) == ((args_io + 4 * count - 1) >> 20) && io_to_ea(args_io, &args_ea)
                             ? ppu::mem() + args_ea
                             : nullptr;
        if (args && inc && method >= m::SET_TRANSFORM_CONSTANT && method + 4 * count <= m::SET_TRANSFORM_CONSTANT + 0x80 &&
            !trace_methods) {
          renderer_transform_constants(args, (method - m::SET_TRANSFORM_CONSTANT) >> 2, count);
          for (u32 i = 0; i < count; ++i) g.regs[(method >> 2) + i] = __builtin_bswap32(reinterpret_cast<const u32*>(args)[i]);
        } else {
          for (u32 i = 0; i < count; ++i) {
            u32 mi = noinc ? method : method + 4 * i;
            u32 arg = args ? __builtin_bswap32(reinterpret_cast<const u32*>(args)[i]) : fifo_read(args_io + 4 * i);
            if (!g_method_special[mi >> 2] && !trace_methods) {  // plain state: the register write only
              record_method(mi, arg);
              g.regs[mi >> 2] = arg;
              continue;
            }
            execute_method(mi >> 2, arg, count, i);
          }
        }
        bump(g.commands);
      }
    }
    *ctrl_get = __builtin_bswap32(get);
    u32 ring = call_stack.empty() ? get : call_stack.front();
    g_ring_get.store(ring, std::memory_order_release);
    if (g_get_waiters.load(std::memory_order_acquire)) {
      u32 lo = g_get_wait_lo.load(std::memory_order_relaxed), hi = g_get_wait_hi.load(std::memory_order_relaxed);
      if ((ring < lo || ring > hi) && (get < lo || get > hi)) WakeByAddressAll(&g_ring_get);
    }
  }
}

}  // namespace

// A PPU thread waiting for the FIFO to leave the command ring range [lo, hi] (io offsets): libgcm's
// fragment callback, before the game overwrites the next fragment. The FIFO's position in the ring
// counts, not GET alone: while the FIFO runs a command list CALLed from that fragment, GET is
// elsewhere but the FIFO returns into the fragment afterwards (releasing it then let the game
// overwrite commands the FIFO had yet to finish: it resumed in the middle of new commands and
// executed their arguments, run 20261012-161028). The thread sleeps until the FIFO thread wakes
// it (instead of a Sleep(0) loop that kept a core busy for the whole wait).
void wait_get_outside(u32 lo, u32 hi) {
  auto* ctrl_get = reinterpret_cast<volatile u32*>(ppu::mem() + kControlAddr + 4);
  // Released when neither the ring position nor GET itself (a command list inside the range,
  // called from elsewhere) is in the range.
  auto released = [&](u32 ring) {
    u32 get = __builtin_bswap32(*ctrl_get);
    return (ring < lo || ring > hi) && (get < lo || get > hi);
  };
  if (cfg().gcm_wait_spin) {  // --gcm-wait spin: a Sleep(0) loop (A/B comparisons)
    for (u32 spins = 0; !released(g_ring_get.load(std::memory_order_acquire)); ++spins)
      if (spins > 64) Sleep(0);
    return;
  }
  for (u32 spins = 0; spins < 64; ++spins) {
    if (released(g_ring_get.load(std::memory_order_acquire))) return;
    _mm_pause();
  }
  g_get_wait_lo.store(lo, std::memory_order_relaxed);
  g_get_wait_hi.store(hi, std::memory_order_relaxed);
  g_get_waiters.fetch_add(1, std::memory_order_acq_rel);
  bool hazard = false;
  for (;;) {
    u32 seen = g_ring_get.load(std::memory_order_acquire);
    if (released(seen)) break;
    u32 get = __builtin_bswap32(*ctrl_get);
    if (!hazard && (get < lo || get > hi)) {  // GET alone would have released the fragment
      hazard = true;
      static std::atomic<u32> n{0};
      u32 k = ++n;
      if (k <= 3 || (k & (k - 1)) == 0)
        LOG_I("rsx", "fragment wait: GET 0x%08x is outside [0x%08x, 0x%08x] but the FIFO returns into it (ring position 0x%08x): "
              "kept waiting (%u so far)", get, lo, hi, seen, k);
      if (k == 1) {  // the FIFO's recent control flow (diagnostics; read while the FIFO thread runs)
        u32 n = g_flow_n;
        for (u32 i = n > 12 ? n - 12 : 0; i < n; ++i) {
          const FlowEvent e = g_flow[i & 63];
          LOG_I("rsx", "  flow %c at 0x%08x -> 0x%08x", e.kind, e.at, e.to);
        }
      }
    }
    WaitOnAddress(&g_ring_get, &seen, sizeof seen, 2);
  }
  g_get_waiters.fetch_sub(1, std::memory_order_acq_rel);
}

void set_ring(u32 io_lo, u32 io_hi) {
  g_ring_lo = io_lo;
  g_ring_hi = io_hi;
}

void start() {
  auto* ctrl = reinterpret_cast<u32*>(ppu::mem() + kControlAddr);
  (void)ctrl;
  g_intr.start();
  std::thread(fifo_thread).detach();
  std::thread(vblank_thread).detach();
  LOG_I("rsx", "RSX threads started");
}

extern std::atomic<u32> g_acquire_ea, g_acquire_value;

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
