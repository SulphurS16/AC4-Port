// SPU channels and the MFC (DMA engine), following local\research\spu_facts.md §4-§6.
// DMA executes synchronously when the command is issued, so tag groups complete immediately
// except for DMA lists stopped at a stall-and-notify element. Lock-line reservations are
// value-checked: loss is detected by comparing the reserved line with memory, lazily, whenever
// the event state is examined (as in the reference emulator).
#include <cstring>

#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/hle.h"
#include "runtime/log.h"
#include "runtime/reservation.h"
#include "runtime/spu/spu.h"

namespace rt::spu {

bool trace_on(Thread& t, u32 cat) {
  static std::atomic<u32> count{0};
  return t.traced && (cfg().trace_spu_cats & cat) && count.fetch_add(1, std::memory_order_relaxed) < cfg().trace_spu_limit;
}

namespace {

u64 now_tb() { return ppu::timebase(); }

std::string line_hex(const u8* p) {
  std::string s;
  for (int i = 0; i < 128; ++i) s += ac4::strprintf("%02x%s", p[i], (i & 15) == 15 && i != 127 ? " " : "");
  return s;
}
std::string line_diff(const u8* a, const u8* b) {
  std::string s;
  for (int i = 0; i < 128; ++i)
    if (a[i] != b[i]) s += ac4::strprintf(" %02x:%02x>%02x", i, a[i], b[i]);
  return s.empty() ? " (no change)" : s;
}

u32 rd_ls32(const Thread& t, u32 a) { return __builtin_bswap32(*reinterpret_cast<const u32*>(t.ls + (a & (kLsSize - 4)))); }

// ---- decrementer (§4.1) ----
u32 dec_read(const Thread& t) { return t.dec_frozen ? t.dec_value : t.dec_value - u32(now_tb() - t.dec_tb); }
void dec_load(Thread& t, u32 v) {
  t.dec_value = v;
  t.dec_tb = now_tb();
  t.dec_frozen = false;
  t.dec_next_tm = t.dec_tb + u64(v) + 1;  // TM when the value passes below zero
}

// ---- events (§5); callers hold t.m ----
void raise_locked(Thread& t, u32 ev) {
  t.event_stat |= ev;
  if (ev & t.event_mask) t.event_count = 1;
}

// Collects events that are detected lazily: decrementer wrap (TM) and reservation loss (LR).
void poll_locked(Thread& t) {
  if (!t.dec_frozen) {
    u64 now = now_tb();
    if (now >= t.dec_next_tm) {
      raise_locked(t, EV_TM);
      t.dec_next_tm += (((now - t.dec_next_tm) >> 32) + 1) << 32;
    }
  }
  if (t.res_valid && std::memcmp(ppu::mem() + t.res_addr, t.res_data, 128) != 0) {
    t.res_valid = false;
    raise_locked(t, EV_LR);
  }
}

// ---- DMA ----
// Resolves an effective address for SPU DMA to a host pointer (nullptr if unsupported). The
// group-relative SPU thread window (0xF0000000+) targets sibling local stores.
u8* resolve_ea(Thread& t, u32 ea, u32 size, Thread** target_thread, u32* target_off) {
  if (ea >= kThreadBase) {
    u32 idx = (ea - kThreadBase) / kThreadOffset, off = (ea - kThreadBase) % kThreadOffset;
    if (!t.group || idx >= t.group->threads.size() || !t.group->threads[idx]) return nullptr;
    Thread* o = t.group->threads[idx].get();
    if (target_thread) *target_thread = o;
    if (target_off) *target_off = off;
    if (off + size <= kLsSize) return o->ls + off;
    return nullptr;  // problem-state registers are handled by the caller
  }
  return ppu::mem() + ea;
}

void dma(Thread& t, bool get, u32 lsa, u32 ea, u32 size) {
  if (!size) return;
  if (size > 0x4000) {
    LOG_E("spu", "SPU %s: DMA %s of 0x%x bytes exceeds 16 KiB (EA 0x%08x); not performed", t.name.c_str(), get ? "get" : "put",
          size, ea);
    return;
  }
  lsa &= kLsSize - 1;
  Thread* other = nullptr;
  u32 off = 0;
  u8* mem = resolve_ea(t, ea, size, &other, &off);
  if (!mem) {
    if (other && !get && size == 4 && (off == kSnr1Offset || off == kSnr2Offset)) {
      write_snr(*other, off == kSnr1Offset ? 0 : 1, rd_ls32(t, lsa));
      return;
    }
    LOG_E("spu", "SPU %s: DMA %s to unsupported EA 0x%08x size 0x%x", t.name.c_str(), get ? "get" : "put", ea, size);
    return;
  }
  u32 first = std::min(size, kLsSize - lsa);  // local store wraps at 256 KiB
  if (get) {
    std::memcpy(t.ls + lsa, mem, first);
    if (first < size) std::memcpy(t.ls, mem + first, size - first);
    note_ls_write(t, lsa, size);
  } else {
    if (other) note_ls_write(*other, off, size);  // PUT into a sibling SPU's local store
    std::memcpy(mem, t.ls + lsa, first);
    if (first < size) std::memcpy(mem + first, t.ls, size - first);
  }
}

// Runs (or resumes) the list command of `tag` until it ends or stops at a stall-and-notify
// element (§6.3).
void run_list(Thread& t, u32 tag) {
  Thread::ListCmd& c = t.stalled[tag];
  while (c.remaining) {
    u32 w0 = rd_ls32(t, c.list), eal = rd_ls32(t, c.list + 4);
    c.list = (c.list + 8) & 0x3FFF8;
    --c.remaining;
    u32 size = w0 & 0x7FFF;
    dma(t, c.get, c.lsa | (eal & 0xF), eal, size);
    if (t.traced && trace_on(t, 1)) {
      std::string data;
      const u8* p = t.ls + ((c.lsa | (eal & 0xF)) & (kLsSize - 1));
      for (u32 i = 0; i < std::min<u32>(size, 32); ++i) data += ac4::strprintf("%02x", p[i]);
      LOG_I("spu-trace", "%s   list %s elem ea %08x size 0x%x lsa %05x%s: %s", t.name.c_str(), c.get ? "get" : "put", eal, size,
            c.lsa | (eal & 0xF), (w0 & 0x80000000u) ? " [stall]" : "", data.c_str());
    }
    c.lsa = (c.lsa + ((size + 15) & ~15u)) & 0x3FFF0;
    if ((w0 & 0x80000000u) && c.remaining) {
      c.active = true;
      std::lock_guard lk(t.m);
      bool was_empty = t.stall_stat == 0;
      t.stall_stat |= 1u << tag;
      if (was_empty) raise_locked(t, EV_SN);
      return;
    }
  }
  c.active = false;
}

void start_list(Thread& t, bool get) {
  u32 tag = t.mfc_tag;
  Thread::ListCmd& c = t.stalled[tag];
  if (c.active) LOG_E("spu", "SPU %s: list command on tag %u while a stalled list is pending", t.name.c_str(), tag);
  if (t.mfc_size > 0x4000 || (t.mfc_size & 7))
    LOG_E("spu", "SPU %s: DMA list size 0x%x is invalid", t.name.c_str(), t.mfc_size);
  c.get = get;
  c.lsa = t.mfc_lsa & 0x3FFF0;
  c.list = t.mfc_eal & 0x3FFF8;
  c.remaining = std::min<u32>(t.mfc_size, 0x4000) / 8;
  run_list(t, tag);
}

// Tags whose commands are not complete (only stalled lists can be pending).
u32 busy_tags(const Thread& t) {
  u32 b = 0;
  for (u32 i = 0; i < 32; ++i)
    if (t.stalled[i].active) b |= 1u << i;
  return b;
}

// Re-evaluates a pending tag-status request (§4.2).
void update_tag_stat(Thread& t) {
  if (t.tag_stat_valid || !t.tag_update) return;
  u32 done = t.tag_mask & ~busy_tags(t);
  bool ready = t.tag_update == 1 ? true : t.tag_update == 2 ? done != 0 : done == t.tag_mask;
  if (!ready) return;
  t.tag_stat = done;
  t.tag_stat_valid = true;
  t.tag_update = 0;
}

void mfc_atomic(Thread& t, u32 cmd) {
  u32 line = t.mfc_eal & ~127u, lsb = t.mfc_lsa & 0x3FF80;
  u8* mem = resolve_ea(t, line, 128, nullptr, nullptr);
  if (!mem) {
    LOG_E("spu", "SPU %s: atomic command 0x%x on unsupported EA 0x%08x", t.name.c_str(), cmd, t.mfc_eal);
    return;
  }
  switch (cmd) {
    case MFC_GETLLAR: {
      alignas(64) u8 snap[128];
      {
        res::LineLock lk(line);
        std::memcpy(snap, mem, 128);
      }
      {
        std::lock_guard lk(t.m);
        // A reservation that was lost before being replaced still raises LR (§5, rule 3).
        if (t.res_valid) {
          const u8* old = t.res_addr == line ? snap : ppu::mem() + t.res_addr;
          if (std::memcmp(old, t.res_data, 128) != 0) raise_locked(t, EV_LR);
        }
        std::memcpy(t.res_data, snap, 128);
        t.res_addr = line;
        t.res_valid = true;
      }
      std::memcpy(t.ls + lsb, snap, 128);
      note_ls_write(t, lsb, 128);
      if (res::watched(line)) res::watch_record('G', t.id, line, true, nullptr, nullptr);
      t.atomic_stat = ATOMIC_GETLLAR_SUCCESS;
      t.atomic_valid = true;
      if (trace_on(t, 1)) LOG_I("spu-trace", "%s pc %05x GETLLAR ea %08x: %s", t.name.c_str(), t.pc, line, line_hex(snap).c_str());
      return;
    }
    case MFC_PUTLLC: {
      bool ok = false;
      if (trace_on(t, 1))
        LOG_I("spu-trace", "%s pc %05x PUTLLC ea %08x:%s", t.name.c_str(), t.pc, line,
              line_diff(t.res_valid && t.res_addr == line ? t.res_data : mem, t.ls + lsb).c_str());
      {
        std::lock_guard lk(t.m);
        if (t.res_valid && t.res_addr == line) {
          res::LineLock ll(line);
          alignas(8) u8 before[128];
          bool watch = res::watched(line);
          if (watch) std::memcpy(before, mem, 128);
          struct Rec {
            bool on;
            u32 id, line;
            const u8* before;
            u8* mem;
            bool* ok;
            ~Rec() {
              if (on) res::watch_record('P', id, line, *ok, before, mem);
            }
          } rec{watch, t.id, line, before, mem, &ok};
          if (std::memcmp(mem, t.res_data, 128) == 0) {
            // Only the doublewords the SPU changed are written, each by compare-exchange against
            // the reserved value. PPU stores take no line lock: one to another doubleword of the
            // line must not be overwritten with the stale reserved data, and one to a changed
            // doubleword fails the conditional store (as the lost reservation would on hardware).
            const u64* want = reinterpret_cast<const u64*>(t.ls + lsb);
            const u64* old = reinterpret_cast<const u64*>(t.res_data);
            u64* m = reinterpret_cast<u64*>(mem);
            ok = true;
            int k = 0;
            for (; k < 16; ++k) {
              if (want[k] == old[k]) continue;
              u64 expect = old[k];
              if (!__atomic_compare_exchange_n(&m[k], &expect, want[k], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                ok = false;
                break;
              }
            }
            if (!ok)  // undo the doublewords already written
              for (int j = 0; j < k; ++j)
                if (want[j] != old[j]) {
                  u64 expect = want[j];
                  __atomic_compare_exchange_n(&m[j], &expect, old[j], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                }
          }
        }
        if (!ok && t.res_valid && t.res_addr == line && !(t.event_stat & EV_LR)) raise_locked(t, EV_LR);
        t.res_valid = false;
      }
      if (ok) res::notify();
      if (!ok && trace_on(t, 1)) LOG_I("spu-trace", "%s   PUTLLC failed", t.name.c_str());
      t.atomic_stat = ok ? ATOMIC_PUTLLC_SUCCESS : ATOMIC_PUTLLC_FAILURE;
      t.atomic_valid = true;
      return;
    }
    case MFC_PUTLLUC:
    case MFC_PUTQLLUC: {
      if (trace_on(t, 1)) LOG_I("spu-trace", "%s pc %05x PUTLLUC ea %08x:%s", t.name.c_str(), t.pc, line, line_diff(mem, t.ls + lsb).c_str());
      {
        res::LineLock lk(line);
        if (res::watched(line)) {
          alignas(8) u8 before[128];
          std::memcpy(before, mem, 128);
          std::memcpy(mem, t.ls + lsb, 128);
          res::watch_record('U', t.id, line, true, before, mem);
        } else {
          std::memcpy(mem, t.ls + lsb, 128);
        }
      }
      res::notify();
      if (cmd == MFC_PUTLLUC) {  // the queued form writes no atomic status
        t.atomic_stat = ATOMIC_PUTLLUC_SUCCESS;
        t.atomic_valid = true;
      }
      return;
    }
  }
}

void mfc_command(Thread& t, u32 cmd) {
  u32 lsa = t.mfc_lsa & (kLsSize - 1), ea = t.mfc_eal, size = t.mfc_size;
  t.mfc_ring[t.mfc_ring_pos++ & 15] = {cmd, lsa, ea, size};
  ++t.mfc_count;
  u32 op = cmd & 0xff;
  if (op != MFC_GETLLAR && op != MFC_PUTLLC && op != MFC_PUTLLUC && op != MFC_PUTQLLUC && trace_on(t, 1))
    LOG_I("spu-trace", "%s pc %05x MFC 0x%02x lsa %05x ea %08x size 0x%x tag %u", t.name.c_str(), t.pc, cmd, lsa, ea, size, t.mfc_tag);
  switch (cmd & 0xff) {
    case MFC_GET:
    case MFC_GETB:
    case MFC_GETF:
      dma(t, true, lsa, ea, size);
      return;
    case MFC_PUT:
    case MFC_PUTB:
    case MFC_PUTF:
    case MFC_PUTR:
    case MFC_PUTRB:
    case MFC_PUTRF:
      dma(t, false, lsa, ea, size);
      return;
    case MFC_SDCRZ: {
      u8* mem = size ? resolve_ea(t, ea, size, nullptr, nullptr) : nullptr;
      if (mem) std::memset(mem, 0, size);
      return;
    }
    case MFC_SDCRT:
    case MFC_SDCRTST:
      return;  // cache hints
    case MFC_GETL:
    case MFC_GETLB:
    case MFC_GETLF:
      start_list(t, true);
      return;
    case MFC_PUTL:
    case MFC_PUTLB:
    case MFC_PUTLF:
    case MFC_PUTRL:
    case MFC_PUTRLB:
    case MFC_PUTRLF:
      start_list(t, false);
      return;
    case MFC_GETLLAR:
    case MFC_PUTLLC:
    case MFC_PUTLLUC:
    case MFC_PUTQLLUC:
      mfc_atomic(t, cmd & 0xff);
      return;
    case MFC_SNDSIG:
    case MFC_SNDSIGB:
    case MFC_SNDSIGF:
      if (size != 4) LOG_E("spu", "SPU %s: SNDSIG with size %u", t.name.c_str(), size);
      dma(t, false, lsa, ea, 4);
      return;
    case MFC_BARRIER:
    case MFC_EIEIO:
    case MFC_SYNC:
      std::atomic_thread_fence(std::memory_order_seq_cst);
      return;
    default:
      LOG_E("spu", "SPU %s: unsupported MFC command 0x%x at pc 0x%05x", t.name.c_str(), cmd, t.pc);
      return;
  }
}

// Waits for an event with the line-write sequence, bounded by the decrementer deadline and a
// polling interval for reservation loss caused by plain PPU stores.
void wait_event(Thread& t, u32 seq, u32 spins) {
  if (spins < 64) {
    _mm_pause();
    return;
  }
  u64 timeout_us = spins < 512 ? 20 : 1000;
  {
    std::lock_guard lk(t.m);
    if ((t.event_mask & EV_TM) && !t.dec_frozen) {
      u64 now = now_tb();
      u64 left = t.dec_next_tm > now ? (t.dec_next_tm - now) * 1000000ull / 79800000ull : 0;
      timeout_us = std::min(timeout_us, left);
    }
  }
  res::wait(seq, timeout_us);
}

}  // namespace

void prepare_start(Thread& t) {
  // Deploy the segments (§9.5 step 1): the LS is not cleared, so data outside them survives.
  for (const Segment& s : t.segs) {
    u32 ls = s.ls & (kLsSize - 1);
    if (s.type == 1) {  // COPY
      std::memcpy(t.ls + ls, ppu::mem() + s.addr, std::min(s.size, kLsSize - ls));
    } else if (s.type == 2) {  // FILL with a 32-bit value
      u32 v = __builtin_bswap32(s.addr);
      for (u32 o = 0; o + 4 <= s.size && ls + o + 4 <= kLsSize; o += 4) std::memcpy(t.ls + ls + o, &v, 4);
    }
  }
  ++t.code_gen;  // the LS contents changed
  // Reset (step 2).
  for (auto& r : t.gpr) r = ppu::v128{};
  {
    std::lock_guard lk(t.m);
    t.out_mbox_full = t.out_intr_full = false;
    t.event_stat = t.event_mask = t.event_count = 0;
    t.res_valid = false;
  }
  t.tag_mask = t.tag_stat = t.tag_update = 0;
  t.tag_stat_valid = false;
  t.atomic_valid = false;
  t.atomic_stat = 0;
  t.stall_stat = 0;
  for (auto& c : t.stalled) c = {};
  t.mfc_lsa = t.mfc_eah = t.mfc_eal = t.mfc_size = t.mfc_tag = 0;
  t.srr0 = 0;
  t.int_enabled = false;
  if (t.option & 2) {  // SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE: the decrementer tracks ~timebase
    dec_load(t, ~u32(now_tb()));
  } else {
    dec_load(t, 0);
  }
  // r1 = {0x3FFF0, 0, 0, 0} (step 3); r3-r6 = arguments in doubleword 0 (step 4).
  t.gpr[1].u32x[3] = 0x3FFF0;
  for (int k = 0; k < 4; ++k) t.gpr[3 + k].u64x[1] = t.args[k];
  t.pc = t.entry & 0x3FFFC;  // interrupts disabled (step 5)
  t.thread_exited = false;
  t.exit_status = 0;
}

u32 check_interrupt(Thread& t, u32 next) {
  if (!t.int_enabled) return next;
  std::lock_guard lk(t.m);
  poll_locked(t);
  if (!t.event_count) return next;
  t.int_enabled = false;
  t.srr0 = next & 0x3FFFC;
  return 0;  // interrupt vector
}

bool event_pending(Thread& t) {
  std::lock_guard lk(t.m);
  poll_locked(t);
  return t.event_count != 0;
}

// Marks the channel a thread may block on (diagnostics).
struct WaitMark {
  Thread& t;
  WaitMark(Thread& th, u32 ch) : t(th) { t.wait_ch.store(ch, std::memory_order_relaxed); }
  ~WaitMark() { t.wait_ch.store(~0u, std::memory_order_relaxed); }
};

u32 read_channel_impl(Thread& t, u32 ch);

u32 read_channel(Thread& t, u32 ch) {
  WaitMark mark(t, ch);
  ++t.ch_reads;
  u32 v = read_channel_impl(t, ch);
  if (t.traced && ch != SPU_RdDec && ch != MFC_RdTagStat && ch != MFC_RdAtomicStat && trace_on(t, ch == SPU_RdInMbox ? 4 : 2))
    LOG_I("spu-trace", "%s pc %05x rdch %u -> 0x%x", t.name.c_str(), t.pc, ch, v);
  return v;
}

u32 read_channel_impl(Thread& t, u32 ch) {
  switch (ch) {
    case SPU_RdEventStat: {
      for (u32 spins = 0;; ++spins) {
        u32 seq = res::begin_wait();
        {
          std::lock_guard lk(t.m);
          poll_locked(t);
          if (t.event_count || t.stop_request) {
            res::end_wait();
            t.event_count = 0;
            return t.event_stat & t.event_mask;
          }
        }
        wait_event(t, seq, spins);
        res::end_wait();
      }
    }
    case SPU_RdSigNotify1:
    case SPU_RdSigNotify2: {
      u32 i = ch == SPU_RdSigNotify1 ? 0 : 1;
      std::unique_lock lk(t.m);
      t.cv.wait(lk, [&] { return t.snr_valid[i] || t.stop_request; });
      u32 v = t.snr[i];
      t.snr[i] = 0;
      t.snr_valid[i] = 0;
      return v;
    }
    case SPU_RdDec: {
      std::lock_guard lk(t.m);
      return dec_read(t);
    }
    case SPU_RdEventMask:
      return t.event_mask;
    case SPU_RdMachStat:
      return t.int_enabled ? 1u : 0u;
    case SPU_RdSRR0:
      return t.srr0;
    case MFC_RdTagMask:
      return t.tag_mask;
    case MFC_RdTagStat: {
      update_tag_stat(t);
      if (!t.tag_stat_valid) {
        // Hardware would stall forever; report and return the completed set.
        LOG_E("spu", "SPU %s: MFC_RdTagStat without a satisfiable tag update request at pc 0x%05x", t.name.c_str(), t.pc);
        return t.tag_mask & ~busy_tags(t);
      }
      t.tag_stat_valid = false;
      return t.tag_stat;
    }
    case MFC_RdListStallStat: {
      std::lock_guard lk(t.m);
      if (!t.stall_stat) LOG_E("spu", "SPU %s: MFC_RdListStallStat with no stalled list at pc 0x%05x", t.name.c_str(), t.pc);
      u32 v = t.stall_stat;
      t.stall_stat = 0;
      return v;
    }
    case MFC_RdAtomicStat: {
      if (!t.atomic_valid) LOG_E("spu", "SPU %s: MFC_RdAtomicStat with no atomic result at pc 0x%05x", t.name.c_str(), t.pc);
      t.atomic_valid = false;
      return t.atomic_stat;
    }
    case SPU_RdInMbox: {
      std::unique_lock lk(t.m);
      t.cv.wait(lk, [&] { return !t.in_mbox.empty() || t.stop_request; });
      if (t.in_mbox.empty()) return 0;
      u32 v = t.in_mbox.front();
      t.in_mbox.pop_front();
      return v;
    }
    default:
      LOG_E("spu", "SPU %s: read of unsupported channel %u at pc 0x%05x", t.name.c_str(), ch, t.pc);
      return 0;
  }
}

void write_channel(Thread& t, u32 ch, u32 v) {
  ++t.ch_writes;
  if (t.traced && (ch == SPU_WrEventMask || ch == SPU_WrEventAck || ch == SPU_WrOutMbox || ch == SPU_WrOutIntrMbox ||
                   ch == SPU_WrDec || ch == MFC_WrListStallAck) && trace_on(t, ch == SPU_WrOutMbox || ch == SPU_WrOutIntrMbox ? 4 : 2))
    LOG_I("spu-trace", "%s pc %05x wrch %u 0x%x", t.name.c_str(), t.pc, ch, v);
  switch (ch) {
    case SPU_WrEventMask: {
      std::lock_guard lk(t.m);
      if ((v & ~EV_IMPLEMENTED) && !t.logged_bad_mask) {
        t.logged_bad_mask = true;
        LOG_E("spu", "SPU %s: event mask 0x%x includes unimplemented events (0x%x)", t.name.c_str(), v, v & ~EV_IMPLEMENTED);
      }
      t.event_mask = v;
      poll_locked(t);
      if (t.event_stat & t.event_mask) t.event_count = 1;
      return;
    }
    case SPU_WrEventAck: {
      std::lock_guard lk(t.m);
      if ((v & EV_TM) && !(t.event_mask & EV_TM) && !t.dec_frozen) {  // freezes the decrementer
        t.dec_value = dec_read(t);
        t.dec_frozen = true;
      }
      t.event_stat &= ~v;
      if (t.event_stat & t.event_mask) t.event_count = 1;
      return;
    }
    case SPU_WrDec: {
      std::lock_guard lk(t.m);
      poll_locked(t);  // a pending TM is collected first
      dec_load(t, v);
      return;
    }
    case SPU_WrSRR0:
      t.srr0 = v & 0x3FFFC;
      return;
    case MFC_LSA: t.mfc_lsa = v; return;
    case MFC_EAH: t.mfc_eah = v; return;
    case MFC_EAL: t.mfc_eal = v; return;
    case MFC_Size: t.mfc_size = std::min<u32>(v, 0xFFFF); return;
    case MFC_TagID: t.mfc_tag = v & 0x1F; return;
    case MFC_Cmd: mfc_command(t, v); return;
    case MFC_WrTagMask:
      t.tag_mask = v;
      update_tag_stat(t);
      return;
    case MFC_WrTagUpdate:
      if (v > 2) LOG_E("spu", "SPU %s: invalid MFC_WrTagUpdate value %u", t.name.c_str(), v);
      t.tag_stat_valid = false;
      t.tag_update = (v & 3) + 1;  // 1 immediate, 2 any, 3 all
      update_tag_stat(t);
      return;
    case MFC_WrListStallAck: {
      u32 tag = v & 0x1F;
      if (!t.stalled[tag].active) {
        LOG_W("spu", "SPU %s: list stall ack for tag %u without a stalled list", t.name.c_str(), tag);
        return;
      }
      run_list(t, tag);
      update_tag_stat(t);
      return;
    }
    case MFC_WrMSSyncReq:
      LOG_E("spu", "SPU %s: MFC_WrMSSyncReq (multisource sync) is not implemented", t.name.c_str());
      return;
    case SPU_WrOutMbox: {
      WaitMark mark(t, ch);
      std::unique_lock lk(t.m);
      t.cv.wait(lk, [&] { return !t.out_mbox_full || t.stop_request; });
      t.out_mbox = v;
      t.out_mbox_full = true;
      return;
    }
    case SPU_WrOutIntrMbox:
      write_out_intr_mbox(t, v);
      return;
    case SPU_Set_Bkmk_Tag:
    case SPU_PM_Start_Ev:
    case SPU_PM_Stop_Ev:
      return;  // performance-monitor hooks: no effect
    default:
      LOG_E("spu", "SPU %s: write of unsupported channel %u (0x%x) at pc 0x%05x", t.name.c_str(), ch, v, t.pc);
      return;
  }
}

u32 channel_count(Thread& t, u32 ch) {
  switch (ch) {
    case SPU_RdEventStat: {
      std::lock_guard lk(t.m);
      poll_locked(t);
      return t.event_count;
    }
    case SPU_RdSigNotify1:
    case SPU_RdSigNotify2: {
      std::lock_guard lk(t.m);
      return t.snr_valid[ch == SPU_RdSigNotify1 ? 0 : 1];
    }
    case SPU_RdInMbox: {
      std::lock_guard lk(t.m);
      return u32(t.in_mbox.size());
    }
    case SPU_WrOutMbox: {
      std::lock_guard lk(t.m);
      return t.out_mbox_full ? 0 : 1;
    }
    case SPU_WrOutIntrMbox:
      return 1;  // serviced synchronously for SPU threads (§7.5)
    case MFC_Cmd:
      return 16;  // the queue is always empty: transfers complete at issue
    case MFC_RdTagStat:
      update_tag_stat(t);
      return t.tag_stat_valid ? 1 : 0;
    case MFC_RdListStallStat: {
      std::lock_guard lk(t.m);
      return t.stall_stat ? 1 : 0;
    }
    case MFC_RdAtomicStat:
      return t.atomic_valid ? 1 : 0;
    case SPU_WrEventMask: case SPU_WrEventAck: case SPU_WrDec: case SPU_RdDec: case SPU_RdEventMask:
    case MFC_RdTagMask: case SPU_RdMachStat: case SPU_WrSRR0: case SPU_RdSRR0: case MFC_LSA: case MFC_EAH:
    case MFC_EAL: case MFC_Size: case MFC_TagID: case MFC_WrTagMask: case MFC_WrTagUpdate: case MFC_WrListStallAck:
    case SPU_Set_Bkmk_Tag: case SPU_PM_Start_Ev: case SPU_PM_Stop_Ev:
      return 1;
    default:
      LOG_E("spu", "SPU %s: channel count of unsupported channel %u at pc 0x%05x", t.name.c_str(), ch, t.pc);
      return 0;
  }
}

s32 write_in_mbox(Thread& t, u32 v) {
  {
    std::lock_guard lk(t.m);
    if (t.in_mbox.size() >= 4) t.in_mbox.back() = v;  // a full mailbox overwrites the newest entry (§4.3)
    else t.in_mbox.push_back(v);
  }
  t.cv.notify_all();
  return CELL_OK;
}

s32 write_snr(Thread& t, u32 which, u32 v) {
  {
    std::lock_guard lk(t.m);
    bool was_empty = !t.snr_valid[which];
    if (t.snr_or[which] && t.snr_valid[which]) t.snr[which] |= v;
    else t.snr[which] = v;
    t.snr_valid[which] = 1;
    if (was_empty) raise_locked(t, which == 0 ? EV_S1 : EV_S2);
  }
  t.cv.notify_all();
  res::notify_slow();  // wake an SPU blocked in SPU_RdEventStat
  return CELL_OK;
}

void on_thread_stopped(Thread& t) {
  std::lock_guard lk(t.m);
  t.in_mbox.clear();
}

}  // namespace rt::spu
