// SPU thread execution and the LV2-side SPU protocols (local\research\spu_facts.md §7.4-§7.5):
// stop-and-signal codes and the outbound-interrupt-mailbox gateway (send/throw event, event flag
// bits).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/hle.h"
#include "runtime/log.h"
#include "runtime/lv2/sync.h"
#include "runtime/reservation.h"
#include "runtime/spu/spu.h"

namespace rt::lv2 {
u32 spu_port_queue(u32 thread, u32 spup);
u32 spu_bound_queue(u32 thread, u32 spuq);
s32 event_flag_set_from_spu(u32 id, u64 bits);
}  // namespace rt::lv2

namespace rt::spu {

constexpr u64 SYS_SPU_THREAD_EVENT_USER_KEY = 0xFFFFFFFF53505501ull;

void run_thread(Thread& t) {
  try {
    interpret(t);
  } catch (const std::exception& e) {
    LOG_E("spu", "SPU %s aborted: %s", t.name.c_str(), e.what());
  }
}

namespace {

// Pops the outbound mailbox word written by the SPU before a syscall stop / interrupt write.
bool take_out_mbox(Thread& t, u32* v) {
  std::lock_guard lk(t.m);
  if (!t.out_mbox_full) return false;
  *v = t.out_mbox;
  t.out_mbox_full = false;
  t.cv.notify_all();
  return true;
}

bool in_mbox_busy(Thread& t) {
  std::lock_guard lk(t.m);
  return !t.in_mbox.empty();
}

// Replaces the inbound mailbox contents with a syscall response.
void respond(Thread& t, std::initializer_list<u32> vals) {
  {
    std::lock_guard lk(t.m);
    t.in_mbox.clear();
    for (u32 v : vals) t.in_mbox.push_back(v);
  }
  t.cv.notify_all();
}

}  // namespace

void write_out_intr_mbox(Thread& t, u32 value) {
  ++t.intr_events;
  u32 code = value >> 24;
  bool responds = code < 64 || code == 128;
  if (code >= 192 + 1 || (code > 128 && code < 192)) {
    LOG_E("spu", "SPU %s: unknown interrupt mailbox code 0x%08x at pc 0x%05x", t.name.c_str(), value, t.pc);
    return;
  }
  u32 data = 0;
  if (!take_out_mbox(t, &data))
    LOG_E("spu", "SPU %s: interrupt mailbox code 0x%08x without outbound mailbox data", t.name.c_str(), value);
  // A call that responds is not performed while the inbound mailbox still holds data.
  if (responds && in_mbox_busy(t)) {
    respond(t, {u32(CELL_EBUSY)});
    return;
  }
  if (code < 128) {  // 0-63: sys_spu_thread_send_event, 64-127: sys_spu_thread_throw_event
    u32 spup = code & 63;
    u32 q = lv2::spu_port_queue(t.id, spup);
    s32 r = q ? lv2::event_queue_send(q, SYS_SPU_THREAD_EVENT_USER_KEY, t.id, (u64(spup) << 32) | (value & 0x00ffffff), data)
              : CELL_ENOTCONN;
    LOG_D("spu", "SPU %s: %s port %u (0x%06x, 0x%08x) -> queue 0x%x: 0x%x", t.name.c_str(), code < 64 ? "send_event" : "throw_event",
          spup, value & 0x00ffffff, data, q, u32(r));
    if (code < 64) respond(t, {u32(r)});
    else if (r != CELL_OK) LOG_W("spu", "SPU %s: throw_event on port %u failed (0x%x)", t.name.c_str(), spup, u32(r));
    return;
  }
  // 128: sys_event_flag_set_bit, 192: sys_event_flag_set_bit_impatient (no response).
  u32 bit = value & 0xffffff;
  s32 r = bit < 64 ? lv2::event_flag_set_from_spu(data, 1ull << bit) : CELL_EINVAL;
  if (code == 128) respond(t, {u32(r)});
  else if (r != CELL_OK) LOG_W("spu", "SPU %s: event flag 0x%x set bit %u failed (0x%x)", t.name.c_str(), data, bit, u32(r));
}

bool stop_and_signal(Thread& t, u32 code) {
  ++t.stops;
  if (trace_on(t, 4)) LOG_I("spu-trace", "%s pc %05x stop 0x%x", t.name.c_str(), t.pc, code);
  switch (code) {
    case 0x001:  // meaning unknown; the reference emulator sleeps 1 ms and continues
      Sleep(1);
      t.pc += 4;
      return true;
    case 0x100: {  // SYS_SPU_THREAD_STOP_YIELD
      u32 dummy;
      if (take_out_mbox(t, &dummy)) LOG_W("spu", "SPU %s: yield with outbound mailbox data 0x%x", t.name.c_str(), dummy);
      SwitchToThread();
      t.pc += 4;
      return true;
    }
    case 0x101: {  // SYS_SPU_THREAD_STOP_GROUP_EXIT
      u32 status = 0;
      if (!take_out_mbox(t, &status)) LOG_W("spu", "SPU %s: group exit without a status word", t.name.c_str());
      Group* g = t.group;
      {
        std::lock_guard lk(g->m);
        if (!g->join_cause) {  // the first terminator wins
          g->join_cause = JOIN_GROUP_EXIT;
          g->exit_status = status;
        }
      }
      for (auto& o : g->threads)
        if (o) {
          o->stop_request = true;
          o->cv.notify_all();
        }
      res::notify_slow();
      LOG_D("spu", "SPU %s: group exit (status 0x%x)", t.name.c_str(), status);
      return false;
    }
    case 0x102: {  // SYS_SPU_THREAD_STOP_THREAD_EXIT
      u32 status = 0;
      if (!take_out_mbox(t, &status)) LOG_W("spu", "SPU %s: thread exit without a status word", t.name.c_str());
      t.exit_status = status;
      t.thread_exited = true;
      LOG_I("spu", "SPU %s: thread exit (status 0x%x)", t.name.c_str(), status);
      return false;
    }
    case 0x110:    // SYS_SPU_THREAD_STOP_RECEIVE_EVENT
    case 0x111: {  // SYS_SPU_THREAD_STOP_TRY_RECEIVE_EVENT
      u32 spuq = 0;
      bool have;
      {
        std::lock_guard lk(t.m);
        have = t.out_mbox_full;
        spuq = t.out_mbox;
      }
      t.pc += 4;
      if (!have) {
        LOG_E("spu", "SPU %s: receive_event without a queue number", t.name.c_str());
        respond(t, {u32(CELL_EINVAL)});
        return true;
      }
      if (in_mbox_busy(t)) {  // the queue number stays in the outbound mailbox in this case
        respond(t, {u32(CELL_EBUSY)});
        return true;
      }
      u32 q = lv2::spu_bound_queue(t.id, spuq);
      if (!q) {
        take_out_mbox(t, &spuq);
        respond(t, {u32(CELL_EINVAL)});
        return true;
      }
      u64 src, d1, d2, d3;
      s32 r = lv2::event_queue_receive_host(q, code == 0x111, &src, &d1, &d2, &d3, &t.stop_request);
      if (t.stop_request) return false;
      take_out_mbox(t, &spuq);
      if (r == CELL_OK) respond(t, {u32(CELL_OK), u32(d1), u32(d2), u32(d3)});
      else respond(t, {u32(r)});
      return true;
    }
    default:
      LOG_E("spu", "SPU %s: unhandled stop code 0x%x at pc 0x%05x; stopping the thread", t.name.c_str(), code, t.pc);
      t.exit_status = code;
      return false;
  }
}

}  // namespace rt::spu
