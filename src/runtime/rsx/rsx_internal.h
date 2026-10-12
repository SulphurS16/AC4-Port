// RSX internals shared by the FIFO, method execution, transfer engines and the renderer.
#pragma once
#include <atomic>

#include "runtime/rsx/rsx.h"

namespace rt::rsx {

// Increments a counter that only the FIFO thread writes (others read it): a plain load and store,
// not a locked read-modify-write, on the per-method and per-draw paths.
inline void bump(std::atomic<u64>& a) { a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

struct State {
  u32 regs[0x10000 / 4] = {};
  std::atomic<u64> commands{0}, flips{0}, draws{0}, transfers{0};  // written by the FIFO thread only (bump)
};
extern State g;

inline u32 reg(u32 method) { return g.regs[method >> 2]; }

// Executes one method write (register index = method >> 2). `count`/`index` describe the
// position inside the current FIFO packet (inline arrays, program loads).
void execute_method(u32 reg_index, u32 arg, u32 count, u32 index);
void do_flip(u32 id);

// DMA handle -> location (0 local, 1 main).
inline u32 dma_location(u32 handle) { return handle == 0xfeed0001 || handle == 0xbad68000 ? 1 : 0; }

// Renderer hooks (implemented by the host renderer; called on the FIFO thread).
void renderer_thread_init();
void renderer_idle();
void present_buffer(u32 id, const DisplayBuffer& b);
void renderer_method(u32 reg_index, u32 arg, u32 count, u32 index);  // state/draw methods
void renderer_memory_written(u32 ea, u32 size);  // CPU-side transfer wrote guest memory
// Guest-write collection for the renderer's caches: the FIFO put pointer read right before the
// renderer's last collection of guest writes. Commands past it may read data written later, so
// reaching it, like a completed semaphore acquire, calls renderer_guest_sync().
extern u32 g_covered_put;
// Time the FIFO thread spent waiting for commands (frame pacing diagnostics).
extern std::atomic<u64> g_fifo_idle_us;
void renderer_guest_sync();
// Where the FIFO thread's time goes, per frame in stall lines: the current phase, switched on the
// FIFO thread only. Methods covers everything not in another phase (including FIFO idle time,
// which is subtracted when reported).
enum Phase {
  kPhMethods, kPhSurfaces, kPhShaders, kPhTextures, kPhVertices, kPhWriteChecks, kPhDraws, kPhFlip, kPhWaits,
  kPhState, kPhConstants, kPhUploads, kPhCount
};
int phase_switch(int p);  // returns the previous phase
struct PhaseScope {
  int prev;
  explicit PhaseScope(int p) : prev(phase_switch(p)) {}
  ~PhaseScope() { phase_switch(prev); }
};
void renderer_flush();                          // finish pending GPU work before a sync point
u32 renderer_report_value(u32 type);            // value for GET_REPORT (e.g. ZPASS_PIXEL_CNT)

}  // namespace rt::rsx
