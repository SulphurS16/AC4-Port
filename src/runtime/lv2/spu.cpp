// LV2 SPU syscalls: SPU thread groups, SPU threads, event connections
// (local\research\spu_facts.md §7.6, §8, §9.4-§9.5).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "runtime/config.h"
#include "runtime/profiler.h"
#include "runtime/lv2/lv2.h"
#include "runtime/lv2/sync.h"
#include "runtime/reservation.h"
#include "runtime/spu/spu.h"
#include "runtime/spu/spu_image.h"

namespace rt::lv2 {

IdMap<spu::Group> g_spu_groups(0x04000100);
IdMap<spu::Thread> g_spu_threads(0x02000000);

namespace {

constexpr u64 SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY = 0xFFFFFFFF53505500ull;

struct GroupAttr {
  be32 nsize, name, type, ct;
};
struct ThreadAttr {
  be32 name, name_len, option;
};
struct ThreadArg {
  be64 arg1, arg2, arg3, arg4;
};

s32 sys_spu_initialize(u32 max_usable, u32 max_raw) {
  if (max_raw > 5 || max_usable > 6) return CELL_EINVAL;
  // The limits are not enforced: thread groups take what they ask for (raw SPUs are not supported).
  LOG_I("spu", "sys_spu_initialize(max_usable %u, max_raw %u)", max_usable, max_raw);
  return CELL_OK;
}

// Whether SPUs may take lock-line reservations on `addr` (access rights: SPU thread 0x2, raw SPU 0x1).
s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags) {
  if (!flags || (flags & ~0x3ull)) return CELL_EINVAL;
  switch (addr >> 28) {
    case 0x0: case 0x1: case 0x2: case 0x3: case 0xc: case 0xe:
      return CELL_OK;
    case 0xf:
      return (flags & 0x1) ? CELL_EPERM : CELL_OK;
    case 0xd:
      return CELL_EPERM;  // PPU stacks
    default:
      return addr >= 0x40000000 && addr < 0xC0000000 ? CELL_OK : CELL_EINVAL;
  }
}

s32 sys_spu_thread_group_create(Ptr<be32> id, u32 num, s32 prio, Ptr<GroupAttr> attr) {
  if (!id || !attr) return CELL_EFAULT;
  if (!num || num > 6 || attr->nsize > 0x80) return CELL_EINVAL;
  u32 type = attr->type;
  if (!(type & 0x08) && !(type & 0x20) && (prio < 16 || prio > 255)) return CELL_EINVAL;
  if (type & ~0x2Eu) LOG_W("spu", "sys_spu_thread_group_create: unusual group type 0x%x", type);
  auto g = std::make_shared<spu::Group>();
  g->num = num;
  g->prio = prio;
  g->type = type;
  if (attr->name) g->name = read_cstr(attr->name, std::max<u32>(attr->nsize, 1));
  g->threads.resize(num);
  u32 gid = g_spu_groups.add(g);
  g->id = gid;
  *id = gid;
  LOG_I("spu", "sys_spu_thread_group_create('%s', %u threads, prio %d, type 0x%x) -> 0x%x", g->name.c_str(), num, prio, type,
        gid);
  return CELL_OK;
}

s32 sys_spu_thread_initialize(Ptr<be32> thread, u32 group_id, u32 spu_num, u32 img, Ptr<ThreadAttr> attr,
                              Ptr<ThreadArg> arg) {
  if (!thread || !img || !attr || !arg) return CELL_EFAULT;
  if (attr->name_len > 0x80 || (attr->option & ~0x3u)) return CELL_EINVAL;
  auto g = g_spu_groups.get(group_id);
  if (!g) return CELL_ESRCH;
  u32 entry = 0;
  std::vector<spu::Segment> segs;
  if (s32 e = spu::image_segments(img, &entry, &segs)) return e;
  std::lock_guard lk(g->m);
  if (spu_num >= g->num) return CELL_EINVAL;
  if (g->run_state != spu::GROUP_NOT_INITIALIZED || g->threads[spu_num]) return CELL_EBUSY;
  auto t = std::make_shared<spu::Thread>();
  std::memset(t->ls, 0, sizeof(t->ls));  // LS starts zeroed at allocation
  t->group = g.get();
  t->index = spu_num;
  t->option = attr->option;
  t->name = attr->name ? read_cstr(attr->name, std::max<u32>(attr->name_len, 1)) : std::string("spu");
  t->entry = entry;
  t->segs = std::move(segs);
  t->args[0] = arg->arg1;
  t->args[1] = arg->arg2;
  t->args[2] = arg->arg3;
  t->args[3] = arg->arg4;
  u32 tid = g_spu_threads.add(t);
  t->id = tid;
  g->threads[spu_num] = t;
  if (++g->init_count == g->num) g->run_state = spu::GROUP_INITIALIZED;
  *thread = tid;
  LOG_I("spu", "sys_spu_thread_initialize('%s' -> 0x%x, group 0x%x #%u, entry 0x%x, %zu segs, args %llx %llx %llx %llx)",
        t->name.c_str(), tid, group_id, spu_num, t->entry, t->segs.size(), (unsigned long long)t->args[0],
        (unsigned long long)t->args[1], (unsigned long long)t->args[2], (unsigned long long)t->args[3]);
  return CELL_OK;
}

s32 sys_spu_thread_set_argument(u32 id, Ptr<ThreadArg> arg) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if (!arg) return CELL_EFAULT;
  t->args[0] = arg->arg1;
  t->args[1] = arg->arg2;
  t->args[2] = arg->arg3;
  t->args[3] = arg->arg4;
  return CELL_OK;
}

DWORD WINAPI spu_host_thread(LPVOID p) {
  auto* t = static_cast<spu::Thread*>(p);
  for (;;) {
    {
      std::unique_lock lk(t->host_m);
      t->host_cv.wait(lk, [&] { return t->host_go || t->host_quit; });
      if (t->host_quit) return 0;
      t->host_go = false;
    }
    spu::run_thread(*t);
    spu::on_thread_stopped(*t);
    spu::Group* g = t->group;
    bool last = false;
    {
      std::lock_guard lk(g->m);
      t->state = 2;
      if (g->running) --g->running;
      if (!g->running) {
        if (!g->join_cause) {
          g->join_cause = spu::JOIN_ALL_THREADS_EXIT;
          g->exit_status = 0;
        }
        g->run_state = spu::GROUP_INITIALIZED;  // can be restarted
        g->stop_latched = true;
        last = true;
      }
    }
    {
      std::lock_guard lk(t->host_m);
      t->host_busy = false;
    }
    t->host_cv.notify_all();
    if (last) {
      g->cv.notify_all();
      LOG_D("spu", "SPU group 0x%x '%s' stopped (cause %u, status 0x%x)", g->id, g->name.c_str(), g->join_cause, g->exit_status);
    }
  }
}

// Ends a thread's host thread (group destroy).
void quit_host(spu::Thread& t) {
  if (!t.host) return;
  {
    std::lock_guard lk(t.host_m);
    t.host_quit = true;
  }
  t.host_cv.notify_all();
  WaitForSingleObject(static_cast<HANDLE>(t.host), INFINITE);
  CloseHandle(static_cast<HANDLE>(t.host));
  prof::unregister_thread(t.host);
  t.host = nullptr;
}

void stop_threads(spu::Group& g) {
  for (auto& t : g.threads)
    if (t) {
      t->stop_request = true;
      t->cv.notify_all();
    }
  res::notify_slow();
}

s32 sys_spu_thread_group_start(u32 id) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  u32 run_queue;
  {
    std::lock_guard lk(g->m);
    if (g->run_state != spu::GROUP_INITIALIZED) return CELL_ESTAT;
    g->run_state = spu::GROUP_RUNNING;
    g->running = g->num;
    g->join_cause = 0;
    g->exit_status = 0;
    g->stop_latched = false;
    g->terminating = false;
    run_queue = g->event_queue[0];
    for (auto& tp : g->threads) {
      spu::Thread& t = *tp;
      {  // the previous run's host thread has reported its stop (it does not need g->m for that)
        std::unique_lock hl(t.host_m);
        t.host_cv.wait(hl, [&] { return !t.host_busy; });
      }
      spu::prepare_start(t);
      if (spu::trace_on(t, 4)) LOG_I("spu-trace", "%s group start (entry 0x%x)", t.name.c_str(), t.entry);
      t.stop_request = false;
      t.state = 1;
    }
  }
  // The RUN event is delivered at every start (§7.6).
  if (run_queue) event_queue_send(run_queue, SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY, id, 0, 0);
  for (auto& tp : g->threads) {
    spu::Thread& t = *tp;
    if (!t.host) {  // the first start: the host thread stays for later starts
      DWORD hid = 0;
      HANDLE h = CreateThread(nullptr, 16u << 20, spu_host_thread, &t, STACK_SIZE_PARAM_IS_A_RESERVATION, &hid);
      t.host = h;
      std::wstring wn(t.name.begin(), t.name.end());
      SetThreadDescription(h, (L"spu:" + wn).c_str());
      prof::register_thread("spu:" + t.name, h);
    }
    {
      std::lock_guard hl(t.host_m);
      t.host_go = true;
      t.host_busy = true;
    }
    t.host_cv.notify_all();
  }
  LOG_D("spu", "SPU group 0x%x '%s' started", id, g->name.c_str());
  return CELL_OK;
}

s32 sys_spu_thread_group_yield(u32 id) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  std::lock_guard lk(g->m);
  return g->run_state == spu::GROUP_RUNNING ? CELL_OK : CELL_ESTAT;
}

s32 sys_spu_thread_group_terminate(u32 id, s32 value) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  std::unique_lock lk(g->m);
  if (g->terminating) {
    g->cv.wait(lk, [&] { return g->run_state != spu::GROUP_RUNNING; });
    return CELL_ESTAT;
  }
  u32 s = g->run_state;
  if (s <= spu::GROUP_INITIALIZED || s == spu::GROUP_WAITING || s == spu::GROUP_WAITING_AND_SUSPENDED) return CELL_ESTAT;
  g->terminating = true;
  g->join_cause = spu::JOIN_TERMINATED;
  g->exit_status = u32(value);
  stop_threads(*g);
  g->cv.wait(lk, [&] { return g->run_state != spu::GROUP_RUNNING; });
  g->terminating = false;
  return CELL_OK;
}

s32 sys_spu_thread_group_join(u32 id, Ptr<be32> cause, Ptr<be32> status) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  std::unique_lock lk(g->m);
  if (g->run_state == spu::GROUP_NOT_INITIALIZED) return CELL_ESTAT;
  if (g->joining) return CELL_EBUSY;
  g->joining = true;
  if (!g->stop_latched) note_block();
  g->cv.wait(lk, [&] { return g->stop_latched; });
  g->stop_latched = false;
  g->joining = false;
  if (!cause) return CELL_EFAULT;
  *cause = g->join_cause;
  if (!status) return CELL_EFAULT;
  *status = g->exit_status;
  return CELL_OK;
}

s32 sys_spu_thread_group_destroy(u32 id) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  {
    std::lock_guard lk(g->m);
    if (g->run_state > spu::GROUP_INITIALIZED || g->joining) return CELL_EBUSY;
  }
  for (auto& t : g->threads)
    if (t) {
      quit_host(*t);
      g_spu_threads.remove(t->id);
    }
  g_spu_groups.remove(id);
  return CELL_OK;
}

s32 sys_spu_thread_group_set_priority(u32 id, s32 prio) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  if (prio < 16 || prio > 255) return CELL_EINVAL;
  std::lock_guard lk(g->m);
  g->prio = prio;
  return CELL_OK;
}

s32 sys_spu_thread_group_get_priority(u32 id, Ptr<bes32> prio) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  if (!prio) return CELL_EFAULT;
  std::lock_guard lk(g->m);
  *prio = (g->type & 0x20) ? 15 : g->prio;
  return CELL_OK;
}

s32 sys_spu_thread_get_exit_status(u32 id, Ptr<bes32> status) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if (t->state != 2 || !t->thread_exited) return CELL_ESTAT;
  if (!status) return CELL_EFAULT;
  *status = s32(t->exit_status);
  return CELL_OK;
}

// LS access is only allowed while the group is started (states WAITING..RUNNING).
bool group_started(spu::Thread& t) {
  std::lock_guard lk(t.group->m);
  return t.group->run_state >= spu::GROUP_WAITING && t.group->run_state <= spu::GROUP_RUNNING;
}

s32 sys_spu_thread_write_ls(u32 id, u32 lsa, u64 value, u32 type) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if ((type != 1 && type != 2 && type != 4 && type != 8) || lsa >= spu::kLsSize || (lsa & (type - 1))) return CELL_EINVAL;
  if (!group_started(*t)) return CELL_ESTAT;
  u8* p = t->ls + lsa;
  spu::note_ls_write(*t, lsa, type);
  switch (type) {
    case 1: *p = u8(value); break;
    case 2: { u16 v = __builtin_bswap16(u16(value)); std::memcpy(p, &v, 2); break; }
    case 4: { u32 v = __builtin_bswap32(u32(value)); std::memcpy(p, &v, 4); break; }
    case 8: { u64 v = __builtin_bswap64(value); std::memcpy(p, &v, 8); break; }
  }
  return CELL_OK;
}

s32 sys_spu_thread_read_ls(u32 id, u32 lsa, Ptr<be64> value, u32 type) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if ((type != 1 && type != 2 && type != 4 && type != 8) || lsa >= spu::kLsSize || (lsa & (type - 1))) return CELL_EINVAL;
  if (!group_started(*t)) return CELL_ESTAT;
  if (!value) return CELL_EFAULT;
  const u8* p = t->ls + lsa;
  u64 v = 0;
  switch (type) {
    case 1: v = *p; break;
    case 2: { u16 x; std::memcpy(&x, p, 2); v = __builtin_bswap16(x); break; }
    case 4: { u32 x; std::memcpy(&x, p, 4); v = __builtin_bswap32(x); break; }
    case 8: { u64 x; std::memcpy(&x, p, 8); v = __builtin_bswap64(x); break; }
  }
  *value = v;
  return CELL_OK;
}

s32 sys_spu_thread_write_spu_mb(u32 id, u32 value) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  return spu::write_in_mbox(*t, value);
}

s32 sys_spu_thread_write_snr(u32 id, s32 number, u32 value) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if (number < 0 || number > 1) return CELL_EINVAL;
  return spu::write_snr(*t, u32(number), value);
}

s32 sys_spu_thread_set_spu_cfg(u32 id, u64 value) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if (value > 3) return CELL_EINVAL;
  std::lock_guard lk(t->m);
  t->snr_or[0] = value & 1;  // bit 0: SNR1 OR mode, bit 1: SNR2 OR mode (reference mapping, §7.6)
  t->snr_or[1] = (value >> 1) & 1;
  return CELL_OK;
}

s32 sys_spu_thread_get_spu_cfg(u32 id, Ptr<be64> value) {
  auto t = g_spu_threads.get(id);
  if (!t) return CELL_ESRCH;
  if (!value) return CELL_EFAULT;
  std::lock_guard lk(t->m);
  *value = u64(t->snr_or[0]) | (u64(t->snr_or[1]) << 1);
  return CELL_OK;
}

}  // namespace

// Decodes the control block of a SPURS instance (layout of libsre's CellSpurs; diagnostics only).
void spurs_dump(u32 a) {
  auto hex = [&](u32 off, u32 n) {
    std::string s;
    for (u32 i = 0; i < n; ++i) s += ac4::strprintf("%02x", ppu::r8(a + off + i));
    return s;
  };
  LOG_I("dump", "SPURS 0x%08x: readyCount1 %s readyCount2/idle %s", a, hex(0x00, 16).c_str(), hex(0x10, 16).c_str());
  LOG_I("dump", "  contention cur %s pend %s min %s max %s", hex(0x20, 16).c_str(), hex(0x30, 16).c_str(), hex(0x40, 16).c_str(),
        hex(0x50, 16).c_str());
  LOG_I("dump", "  wklFlag %s signal1 %04x sysSrvMsg %02x spuIdling %02x flags1 %02x nSpus %u flagReceiver %02x signal2 %04x",
        hex(0x60, 16).c_str(), ppu::r16(a + 0x70), ppu::r8(a + 0x72), ppu::r8(a + 0x73), ppu::r8(a + 0x74), ppu::r8(a + 0x76),
        ppu::r8(a + 0x77), ppu::r16(a + 0x78));
  LOG_I("dump", "  state1 %s status1 %s event1 %s enabled %08x msgUpdateWkl %02x sysSrvOnSpu %02x", hex(0x80, 16).c_str(),
        hex(0x90, 16).c_str(), hex(0xA0, 16).c_str(), ppu::r32(a + 0xB0), ppu::r8(a + 0xBD), ppu::r8(a + 0xC8));
  for (u32 i = 0; i < 16; ++i) {
    u32 w = a + 0xB00 + 0x20 * i;
    if (!ppu::r64(w)) continue;
    LOG_I("dump", "  wkl %u: addr 0x%llx arg 0x%llx size 0x%x uid %u prio %s", i, (unsigned long long)ppu::r64(w),
          (unsigned long long)ppu::r64(w + 8), ppu::r32(w + 0x10), ppu::r8(w + 0x14), hex(0xB00 + 0x20 * i + 0x18, 8).c_str());
    // If the argument is a taskset (its spurs pointer at +0x60 points back here), decode it.
    u32 ts = u32(ppu::r64(w + 8));
    if (ts && ts < 0xF0000000 && u32(ppu::r64(ts + 0x60)) == a) {
      auto h = [&](u32 off) {
        return ac4::strprintf("%08x%08x%08x%08x", ppu::r32(ts + off), ppu::r32(ts + off + 4), ppu::r32(ts + off + 8),
                               ppu::r32(ts + off + 12));
      };
      LOG_I("dump", "    taskset 0x%08x: running %s ready %s pending %s", ts, h(0x00).c_str(), h(0x10).c_str(), h(0x20).c_str());
      LOG_I("dump", "      enabled %s signalled %s waiting %s", h(0x30).c_str(), h(0x40).c_str(), h(0x50).c_str());
      LOG_I("dump", "      args 0x%llx clear_ls %u x71 %u wkl_flag_wait %u last %u wid %u", (unsigned long long)ppu::r64(ts + 0x68),
            ppu::r8(ts + 0x70), ppu::r8(ts + 0x71), ppu::r8(ts + 0x72), ppu::r8(ts + 0x73), ppu::r32(ts + 0x74));
      for (u32 k = 0; k < 128; ++k) {
        if (!(ppu::r32(ts + 0x30 + 4 * (k / 32)) & (0x80000000u >> (k % 32)))) continue;
        u32 ti = ts + 0x80 + 0x30 * k;
        LOG_I("dump", "      task %u: args %016llx%016llx elf 0x%llx ctx 0x%llx ls %016llx%016llx", k,
              (unsigned long long)ppu::r64(ti), (unsigned long long)ppu::r64(ti + 8), (unsigned long long)ppu::r64(ti + 0x10),
              (unsigned long long)ppu::r64(ti + 0x18), (unsigned long long)ppu::r64(ti + 0x20),
              (unsigned long long)ppu::r64(ti + 0x28));
      }
    }
  }
  // Handler state as this firmware's libsre lays it out (its handler loop reads mutex 0xD54,
  // cond 0xD58, dirty 0xD69, waiting 0xD6A, exiting 0xD6B, exception 0xD70).
  LOG_I("dump", "  handler dirty %u waiting %u exiting %u, mutex 0x%x cond 0x%x, exception 0x%x, spuTG 0x%x", ppu::r8(a + 0xD69),
        ppu::r8(a + 0xD6A), ppu::r8(a + 0xD6B), ppu::r32(a + 0xD54), ppu::r32(a + 0xD58), ppu::r32(a + 0xD70), ppu::r32(a + 0xD30));
}

void spu_dump() {
  std::vector<u32> spurs;
  g_spu_groups.for_each([&](u32, spu::Group& g) {
    if (g.name.find("CellSpursKernelGroup") != std::string::npos && !g.threads.empty() && g.threads[0])
      spurs.push_back(u32(g.threads[0]->args[1]));
  });
  for (u32 a : spurs) spurs_dump(a);
  g_spu_groups.for_each([](u32 gid, spu::Group& g) {
    LOG_I("dump", "SPU group 0x%x '%s': state %u, running %u", gid, g.name.c_str(), g.run_state, g.running);
    for (auto& t : g.threads) {
      if (!t) continue;
      u32 ch = t->wait_ch.load();
      LOG_I("dump", "  SPU 0x%x '%s': state %d, pc 0x%05x, %llu steps, %llu MFC cmds, %llu/%llu ch reads/writes, %llu events, %llu stops%s",
            t->id, t->name.c_str(), int(t->state), t->pc, (unsigned long long)t->steps, (unsigned long long)t->mfc_count,
            (unsigned long long)t->ch_reads, (unsigned long long)t->ch_writes, (unsigned long long)t->intr_events,
            (unsigned long long)t->stops,
            ch == ~0u ? "" : ac4::strprintf(", blocked on channel %u", ch).c_str());
      ULONG64 cycles = 0;
      if (t->host) QueryThreadCycleTime(static_cast<HANDLE>(t->host), &cycles);
      if (cycles)
        LOG_I("dump", "    host thread: %.2f G cycles, %.1f cycles per SPU instruction", double(cycles) / 1e9,
              double(cycles) / double(t->steps ? t->steps : 1));
      std::map<u32, u32> hist;
      for (u32 pc : t->pc_ring)
        if (pc) ++hist[pc];
      std::string pcs;
      for (auto& [pc, n] : hist) pcs += ac4::strprintf(" %05x:%u", pc, n);
      LOG_I("dump", "    sampled pcs:%s", pcs.c_str());
      std::vector<std::pair<u64, u32>> ops;
      for (u32 i = 0; i < 256; ++i)
        if (t->op_hist[i]) ops.push_back({t->op_hist[i], i});
      std::sort(ops.rbegin(), ops.rend());
      std::string top;
      for (size_t i = 0; i < ops.size() && i < 24; ++i)
        top += ac4::strprintf(" %s:%.1f%%", spu::op_name(ops[i].second), 100.0 * double(ops[i].first) / double(t->steps ? t->steps : 1));
      LOG_I("dump", "    top ops:%s", top.c_str());
      for (u32 i = 0; i < 16; ++i) {
        const auto& r = t->mfc_ring[(t->mfc_ring_pos + i) & 15];
        if (r.cmd) LOG_I("dump", "    mfc cmd 0x%02x lsa 0x%05x ea 0x%08x size 0x%x", r.cmd, r.lsa, r.ea, r.size);
      }
    }
  });
}

// Per-thread event port connections (SPU -> PPU queues) and bound queues (PPU -> SPU).
struct SpuConn {
  std::mutex m;
  std::map<u32, std::map<u32, u32>> ports;  // thread id -> spup -> queue
  std::map<u32, std::map<u32, u32>> bound;  // thread id -> spuq number -> queue
};
SpuConn g_conn;

u32 spu_port_queue(u32 thread, u32 spup) {
  std::lock_guard lk(g_conn.m);
  auto it = g_conn.ports.find(thread);
  if (it == g_conn.ports.end()) return 0;
  auto jt = it->second.find(spup);
  return jt == it->second.end() ? 0 : jt->second;
}
u32 spu_bound_queue(u32 thread, u32 spuq) {
  std::lock_guard lk(g_conn.m);
  auto it = g_conn.bound.find(thread);
  if (it == g_conn.bound.end()) return 0;
  auto jt = it->second.find(spuq);
  return jt == it->second.end() ? 0 : jt->second;
}

namespace {

s32 sys_spu_thread_connect_event(u32 id, u32 eq, u32 et, u32 spup) {
  if (!g_spu_threads.get(id) || !event_queue_exists(eq)) return CELL_ESRCH;
  if (et != 1 /*SYS_SPU_THREAD_EVENT_USER*/ || spup > 63) {
    LOG_W("spu", "sys_spu_thread_connect_event: event type %u / spup %u not supported", et, spup);
    return CELL_EINVAL;
  }
  std::lock_guard lk(g_conn.m);
  auto& m = g_conn.ports[id];
  if (m.count(spup)) return CELL_EISCONN;
  m[spup] = eq;
  return CELL_OK;
}
s32 sys_spu_thread_disconnect_event(u32 id, u32 et, u32 spup) {
  if (!g_spu_threads.get(id)) return CELL_ESRCH;
  if (et != 1 || spup > 63) return CELL_EINVAL;
  std::lock_guard lk(g_conn.m);
  if (!g_conn.ports[id].erase(spup)) return CELL_ENOTCONN;
  return CELL_OK;
}
s32 sys_spu_thread_bind_queue(u32 id, u32 spuq, u32 spuq_num) {
  if (!g_spu_threads.get(id) || !event_queue_exists(spuq)) return CELL_ESRCH;
  if (event_queue_type(spuq) != 2 /*SYS_SPU_QUEUE*/) return CELL_EINVAL;
  std::lock_guard lk(g_conn.m);
  auto& m = g_conn.bound[id];
  if (m.size() >= 32) return CELL_EAGAIN;
  if (m.count(spuq_num)) return CELL_EBUSY;
  for (auto& [num, q] : m)
    if (q == spuq) return CELL_EBUSY;
  m[spuq_num] = spuq;
  return CELL_OK;
}
s32 sys_spu_thread_unbind_queue(u32 id, u32 spuq_num) {
  if (!g_spu_threads.get(id)) return CELL_ESRCH;
  std::lock_guard lk(g_conn.m);
  if (!g_conn.bound[id].erase(spuq_num)) return CELL_ESRCH;
  return CELL_OK;
}
int group_event_slot(u32 et) { return et == 1 ? 0 : et == 2 ? 1 : et == 4 ? 2 : -1; }
s32 sys_spu_thread_group_connect_event(u32 id, u32 eq, u32 et) {
  auto g = g_spu_groups.get(id);
  if (!g || !event_queue_exists(eq)) return CELL_ESRCH;
  int slot = group_event_slot(et);
  if (slot < 0) return CELL_EINVAL;
  std::lock_guard lk(g->m);
  if (g->event_queue[slot]) return CELL_EBUSY;
  g->event_queue[slot] = eq;
  return CELL_OK;
}
s32 sys_spu_thread_group_disconnect_event(u32 id, u32 et) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  int slot = group_event_slot(et);
  if (slot < 0) return CELL_EINVAL;
  std::lock_guard lk(g->m);
  if (!g->event_queue[slot]) return CELL_ENOTCONN;
  g->event_queue[slot] = 0;
  return CELL_OK;
}
// Connects the lowest port in `req` that is free on every thread of the group to queue `eq`.
s32 sys_spu_thread_group_connect_event_all_threads(u32 id, u32 eq, u64 req, Ptr<u8> spup) {
  auto g = g_spu_groups.get(id);
  if (!g || !event_queue_exists(eq)) return CELL_ESRCH;
  if (!req) return CELL_EINVAL;
  {
    std::lock_guard glk(g->m);
    if (g->run_state < spu::GROUP_INITIALIZED) return CELL_ESTAT;
  }
  std::lock_guard lk(g_conn.m);
  for (u32 port = 0; port < 64; ++port) {
    if (!(req & (1ull << port))) continue;
    bool free = true;
    for (auto& t : g->threads)
      if (t && g_conn.ports[t->id].count(port)) free = false;
    if (!free) continue;
    for (auto& t : g->threads)
      if (t) g_conn.ports[t->id][port] = eq;
    *spup = u8(port);
    return CELL_OK;
  }
  return CELL_EISCONN;
}
s32 sys_spu_thread_group_disconnect_event_all_threads(u32 id, u32 spup) {
  auto g = g_spu_groups.get(id);
  if (!g) return CELL_ESRCH;
  if (spup > 63) return CELL_EINVAL;
  std::lock_guard lk(g_conn.m);
  for (auto& t : g->threads)
    if (t) g_conn.ports[t->id].erase(spup);
  return CELL_OK;
}

hle::SyscallTable s_spu({
    SYSCALL(14, sys_process_is_spu_lock_line_reservation_address),
    SYSCALL(165, sys_spu_thread_get_exit_status),
    SYSCALL(166, sys_spu_thread_set_argument),
    SYSCALL(169, sys_spu_initialize),
    SYSCALL(170, sys_spu_thread_group_create),
    SYSCALL(171, sys_spu_thread_group_destroy),
    SYSCALL(172, sys_spu_thread_initialize),
    SYSCALL(173, sys_spu_thread_group_start),
    SYSCALL(176, sys_spu_thread_group_yield),
    SYSCALL(177, sys_spu_thread_group_terminate),
    SYSCALL(178, sys_spu_thread_group_join),
    SYSCALL(179, sys_spu_thread_group_set_priority),
    SYSCALL(180, sys_spu_thread_group_get_priority),
    SYSCALL(181, sys_spu_thread_write_ls),
    SYSCALL(182, sys_spu_thread_read_ls),
    SYSCALL(184, sys_spu_thread_write_snr),
    SYSCALL(185, sys_spu_thread_group_connect_event),
    SYSCALL(186, sys_spu_thread_group_disconnect_event),
    SYSCALL(187, sys_spu_thread_set_spu_cfg),
    SYSCALL(188, sys_spu_thread_get_spu_cfg),
    SYSCALL(190, sys_spu_thread_write_spu_mb),
    SYSCALL(191, sys_spu_thread_connect_event),
    SYSCALL(192, sys_spu_thread_disconnect_event),
    SYSCALL(193, sys_spu_thread_bind_queue),
    SYSCALL(194, sys_spu_thread_unbind_queue),
    SYSCALL(251, sys_spu_thread_group_connect_event_all_threads),
    SYSCALL(252, sys_spu_thread_group_disconnect_event_all_threads),
});

}  // namespace
}  // namespace rt::lv2
