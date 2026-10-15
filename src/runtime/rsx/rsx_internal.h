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
// Nonzero for methods with an effect beyond the register write (execute_method's own cases, inline
// image data and the renderer's methods): the FIFO thread stores the others directly (set by
// init_method_kinds; most methods of a draw are plain state).
extern u8 g_method_special[0x10000 / 4];
void init_method_kinds();
// The renderer's methods with effects (renderer_method): `mark(method, bytes)` for each.
void renderer_mark_special(void (*mark)(u32 method, u32 bytes));
// The last methods executed, for diagnostics (dump): plain register writes included.
extern u32 g_method_ring[32][2];
extern std::atomic<u32> g_method_pos;
inline void record_method(u32 method, u32 arg) {
  u32 n = g_method_pos.load(std::memory_order_relaxed);
  g_method_ring[n & 31][0] = method;
  g_method_ring[n & 31][1] = arg;
  g_method_pos.store(n + 1, std::memory_order_relaxed);
}
void do_flip(u32 id);
// The FIFO executes words that are not commands (a method argument no command buffer can hold):
// reports the recent FIFO control flow and methods, then stops with `why`.
[[noreturn]] void fifo_desync(const char* why, u32 value);

// DMA handle -> location (0 local, 1 main).
inline u32 dma_location(u32 handle) { return handle == 0xfeed0001 || handle == 0xbad68000 ? 1 : 0; }

// Renderer hooks (implemented by the host renderer; called on the FIFO thread).
void renderer_thread_init();
void renderer_idle();
void present_buffer(u32 id, const DisplayBuffer& b);
void renderer_method(u32 reg_index, u32 arg, u32 count, u32 index);  // state/draw methods
// A burst of SET_TRANSFORM_CONSTANT words (big-endian, as in the command buffer) starting at word
// `first` of the method range: the same effect as renderer_method per word, in one call.
void renderer_transform_constants(const u8* words, u32 first, u32 count);
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
// Value for GET_REPORT of `type` written to guest address `ea` (pixel counts arrive later, see
// renderer.cpp "Pixel counts").
u32 renderer_report_value(u32 type, u32 ea);

}  // namespace rt::rsx
