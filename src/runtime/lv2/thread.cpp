// LV2 PPU thread, time, timer and TTY syscalls.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctime>

#include "runtime/lv2/lv2.h"

namespace rt::lv2 {

u64 system_time_us() {
  static LARGE_INTEGER f, t0;
  static bool init = [] {
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    return true;
  }();
  (void)init;
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  u64 d = u64(n.QuadPart - t0.QuadPart);
  return (d / u64(f.QuadPart)) * 1000000ull + (d % u64(f.QuadPart)) * 1000000ull / u64(f.QuadPart);
}

// High-resolution sleep (sys_timer_usleep): waitable timer per thread.
void sleep_us(u64 us) {
  note_block();
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!us) {
    SwitchToThread();
    return;
  }
  if (!timer) {
    Sleep(DWORD((us + 999) / 1000));
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -s64(us * 10);  // 100 ns units, relative
  SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
  WaitForSingleObject(timer, INFINITE);
}

namespace {

void sys_ppu_thread_exit(u64 val) { thread_exit(val); }
s32 sys_ppu_thread_yield() {
  SwitchToThread();
  return CELL_OK;
}
s32 sys_ppu_thread_join(u64 id, Ptr<be64> status) {
  u64 st = 0;
  s32 r = thread_join(u32(id), &st);
  if (r == CELL_OK && status) *status = st;
  return r;
}
s32 sys_ppu_thread_detach(u64 id) { return thread_detach(u32(id)); }
s32 sys_ppu_thread_set_priority(u64 id, s32 prio) {
  PpuThread* t = thread_get(u32(id));
  if (!t) return CELL_ESRCH;
  if (prio < 0 || prio > 3071) return CELL_EINVAL;
  t->prio = prio;
  return CELL_OK;
}
s32 sys_ppu_thread_get_priority(u64 id, Ptr<bes32> prio) {
  PpuThread* t = thread_get(u32(id));
  if (!t) return CELL_ESRCH;
  *prio = t->prio;
  return CELL_OK;
}

s32 sys_timer_usleep(u64 us) {
  sleep_us(us);
  return CELL_OK;
}
s32 sys_timer_sleep(u32 sec) {
  sleep_us(u64(sec) * 1000000);
  return CELL_OK;
}

s32 sys_time_get_timezone(Ptr<bes32> timezone, Ptr<bes32> summertime) {
  TIME_ZONE_INFORMATION tzi{};
  DWORD r = GetTimeZoneInformation(&tzi);
  if (timezone) *timezone = -s32(tzi.Bias);
  if (summertime) *summertime = r == TIME_ZONE_ID_DAYLIGHT ? -s32(tzi.DaylightBias) : 0;
  return CELL_OK;
}
s32 sys_time_get_current_time(Ptr<bes64> sec, Ptr<bes64> nsec) {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  u64 t = (u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601
  t -= 116444736000000000ull;                                 // -> since 1970
  if (sec) *sec = s64(t / 10000000ull);
  if (nsec) *nsec = s64((t % 10000000ull) * 100);
  return CELL_OK;
}
u64 sys_time_get_timebase_frequency() { return 79800000ull; }

s32 sys_tty_write(s32 ch, u32 buf, u32 len, Ptr<be32> written) {
  if (ch < 0 || ch > 15) return CELL_EINVAL;
  std::string s(reinterpret_cast<const char*>(ppu::mem() + buf), len);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (!s.empty()) LOG_I("tty", "%s", s.c_str());
  if (written) *written = len;
  return CELL_OK;
}
s32 sys_tty_read(s32 ch, u32 buf, u32 len, Ptr<be32> read) {
  if (read) *read = 0;
  return CELL_OK;
}

hle::SyscallTable s_thread({
    SYSCALL(41, sys_ppu_thread_exit),
    SYSCALL(43, sys_ppu_thread_yield),
    SYSCALL(44, sys_ppu_thread_join),
    SYSCALL(45, sys_ppu_thread_detach),
    SYSCALL(47, sys_ppu_thread_set_priority),
    SYSCALL(48, sys_ppu_thread_get_priority),
    SYSCALL(141, sys_timer_usleep),
    SYSCALL(142, sys_timer_sleep),
    SYSCALL(144, sys_time_get_timezone),
    SYSCALL(145, sys_time_get_current_time),
    SYSCALL(147, sys_time_get_timebase_frequency),
    SYSCALL(402, sys_tty_read),
    SYSCALL(403, sys_tty_write),
});

}  // namespace
}  // namespace rt::lv2
