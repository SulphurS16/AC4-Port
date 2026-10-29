// Striped locks over 128-byte guest memory lines. PPU stwcx/stdcx and the SPU lock-line commands
// (GETLLAR/PUTLLC/PUTLLUC) take the lock of the line they access, so an SPU's 128-byte conditional
// store and a PPU's conditional store to the same line cannot interleave.
//
// Line writes made under the lock also bump a global sequence number that SPUs waiting for a
// reservation-lost event sleep on, so they wake promptly instead of polling. (Plain PPU stores
// do not bump it; waiters therefore also poll with a bounded timeout.)
#pragma once
#include <atomic>
#include <immintrin.h>

#include "common/bytes.h"

namespace rt::res {

constexpr u32 kStripes = 1024;
struct alignas(64) Stripe {
  std::atomic<u32> v{0};
};
extern Stripe g_stripes[kStripes];
extern std::atomic<u32> g_seq, g_waiters;

inline Stripe& stripe(u32 ea) { return g_stripes[(ea >> 7) & (kStripes - 1)]; }

class LineLock {
 public:
  explicit LineLock(u32 ea) : s_(stripe(ea)) {
    for (;;) {
      if (!s_.v.exchange(1, std::memory_order_acquire)) return;
      while (s_.v.load(std::memory_order_relaxed)) _mm_pause();
    }
  }
  ~LineLock() { s_.v.store(0, std::memory_order_release); }
  LineLock(const LineLock&) = delete;
  LineLock& operator=(const LineLock&) = delete;

 private:
  Stripe& s_;
};

// Wakes waiters after a line write (cheap when nobody waits).
void notify_slow();
inline void notify() {
  if (g_waiters.load(std::memory_order_seq_cst)) notify_slow();
}
// Waiter protocol: begin_wait() (returns the current sequence), re-check the condition, then
// wait(seq, timeout_us) unless satisfied, then end_wait().
inline u32 begin_wait() {
  g_waiters.fetch_add(1, std::memory_order_seq_cst);
  return g_seq.load(std::memory_order_seq_cst);
}
void wait(u32 seq, u64 timeout_us);
inline void end_wait() { g_waiters.fetch_sub(1, std::memory_order_seq_cst); }
// Logical processors this process may run on (its affinity mask; re-read about once a second):
// waiters spin and yield less where the threads they wait for need those processors.
u32 host_processors();

// Diagnostics (--watch-lines): a ring of the atomic operations on watched address ranges, written
// to the log at shutdown. kind: 'C' PPU conditional store, 'G' GETLLAR, 'P' PUTLLC, 'U' PUTLLUC.
extern std::atomic<int> g_watch_ranges;
bool watched_slow(u32 line);
inline bool watched(u32 ea) { return g_watch_ranges.load(std::memory_order_relaxed) && watched_slow(ea & ~127u); }
void watch_add(u32 begin, u32 size);
// Records the doublewords that differ between `before` and `after` (128-byte line images; null
// images record no change list), with the operation's outcome.
void watch_record(char kind, u32 thread, u32 ea, bool ok, const u8* before, const u8* after);
void watch_dump(const std::string& path);  // writes the recorded operations to `path`

}  // namespace rt::res
