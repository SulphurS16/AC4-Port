// SPU threads (LV2 "SPU thread" model): local store, registers, channels, MFC, and the
// thread-group lifecycle. Each SPU thread runs the SPU interpreter on its own host thread.
// Behaviour follows local\research\spu_facts.md (section numbers referenced below).
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ppu/ppu_context.h"

namespace rt::spu {

constexpr u32 kLsSize = 0x40000;
constexpr u32 kThreadBase = 0xF0000000, kThreadOffset = 0x100000;  // group-relative SPU thread window (§6.5)
constexpr u32 kSnr1Offset = 0x5400C, kSnr2Offset = 0x5C00C;          // signal notification registers

// Channels (§4).
enum : u32 {
  SPU_RdEventStat = 0, SPU_WrEventMask = 1, SPU_WrEventAck = 2, SPU_RdSigNotify1 = 3, SPU_RdSigNotify2 = 4,
  SPU_WrDec = 7, SPU_RdDec = 8, MFC_WrMSSyncReq = 9, SPU_RdEventMask = 11, MFC_RdTagMask = 12, SPU_RdMachStat = 13,
  SPU_WrSRR0 = 14, SPU_RdSRR0 = 15, MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18, MFC_Size = 19, MFC_TagID = 20, MFC_Cmd = 21,
  MFC_WrTagMask = 22, MFC_WrTagUpdate = 23, MFC_RdTagStat = 24, MFC_RdListStallStat = 25, MFC_WrListStallAck = 26,
  MFC_RdAtomicStat = 27, SPU_WrOutMbox = 28, SPU_RdInMbox = 29, SPU_WrOutIntrMbox = 30,
  SPU_Set_Bkmk_Tag = 69, SPU_PM_Start_Ev = 70, SPU_PM_Stop_Ev = 71,
};

// Event bits (§5). Implemented: LR, S1, S2, TM, SN (as in the reference emulator).
enum : u32 {
  EV_MS = 0x1000, EV_A = 0x0800, EV_LR = 0x0400, EV_S1 = 0x0200, EV_S2 = 0x0100, EV_LE = 0x0080, EV_ME = 0x0040,
  EV_TM = 0x0020, EV_MB = 0x0010, EV_QV = 0x0008, EV_SN = 0x0002, EV_TG = 0x0001,
  EV_IMPLEMENTED = EV_LR | EV_S1 | EV_S2 | EV_TM | EV_SN,
};

// MFC commands (§6.1).
enum : u32 {
  MFC_PUT = 0x20, MFC_PUTB = 0x21, MFC_PUTF = 0x22, MFC_PUTL = 0x24, MFC_PUTLB = 0x25, MFC_PUTLF = 0x26,
  MFC_PUTR = 0x30, MFC_PUTRB = 0x31, MFC_PUTRF = 0x32, MFC_PUTRL = 0x34, MFC_PUTRLB = 0x35, MFC_PUTRLF = 0x36,
  MFC_GET = 0x40, MFC_GETB = 0x41, MFC_GETF = 0x42, MFC_GETL = 0x44, MFC_GETLB = 0x45, MFC_GETLF = 0x46,
  MFC_SNDSIG = 0xA0, MFC_SNDSIGB = 0xA1, MFC_SNDSIGF = 0xA2, MFC_PUTLLUC = 0xB0, MFC_PUTLLC = 0xB4, MFC_PUTQLLUC = 0xB8,
  MFC_BARRIER = 0xC0, MFC_EIEIO = 0xC8, MFC_SYNC = 0xCC, MFC_GETLLAR = 0xD0,
  MFC_SDCRT = 0x80, MFC_SDCRTST = 0x81, MFC_SDCRZ = 0x89,
};
enum : u32 { ATOMIC_PUTLLC_SUCCESS = 0, ATOMIC_PUTLLC_FAILURE = 1, ATOMIC_PUTLLUC_SUCCESS = 2, ATOMIC_GETLLAR_SUCCESS = 4 };

// Group run states (§8.2).
enum : u32 {
  GROUP_NOT_INITIALIZED = 0, GROUP_INITIALIZED = 1, GROUP_WAITING = 3, GROUP_SUSPENDED = 4,
  GROUP_WAITING_AND_SUSPENDED = 5, GROUP_RUNNING = 6,
};
// Join causes (§8.3).
enum : u32 { JOIN_GROUP_EXIT = 1, JOIN_ALL_THREADS_EXIT = 2, JOIN_TERMINATED = 4 };

// Segment of an SPU image (sys_spu_segment, §8.2): 1 COPY, 2 FILL, 4 INFO.
struct Segment {
  u32 type, ls, size, addr;
};

struct Group;

struct Thread {
  u32 id = 0;
  std::string name;
  Group* group = nullptr;
  u32 index = 0;   // spu_num within the group
  u32 option = 0;  // sys_spu_thread_attribute option (2 = DEC_SYNC_TB_ENABLE)
  alignas(64) u8 ls[kLsSize + 64];
  alignas(16) ppu::v128 gpr[128];
  u32 pc = 0;
  // Image and arguments, applied at every group start (§9.5).
  u32 entry = 0;
  std::vector<Segment> segs;
  u64 args[4] = {};

  // Channel state shared with other threads (guarded by m).
  std::mutex m;
  std::condition_variable cv;
  std::deque<u32> in_mbox;  // PPU -> SPU, depth 4
  bool out_mbox_full = false, out_intr_full = false;
  u32 out_mbox = 0, out_intr = 0;
  u32 snr[2] = {}, snr_valid[2] = {}, snr_or[2] = {};
  u32 event_mask = 0, event_stat = 0, event_count = 0;
  bool logged_bad_mask = false;

  // State touched only by the SPU's own host thread.
  u32 tag_mask = 0;
  bool tag_stat_valid = false;
  u32 tag_stat = 0, tag_update = 0;
  u32 mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
  bool atomic_valid = false;
  u32 atomic_stat = 0;
  u32 stall_stat = 0;  // tags with a stalled DMA list (MFC_RdListStallStat)
  struct ListCmd {
    bool active = false, get = false;
    u32 lsa = 0, list = 0, remaining = 0;
  } stalled[32];
  u32 srr0 = 0;
  bool int_enabled = false;
  // Decrementer (§4.1): value = dec_value - (timebase - dec_tb) while running.
  u32 dec_value = 0;
  u64 dec_tb = 0, dec_next_tm = 0;
  bool dec_frozen = false;
  // Lock-line reservation (§5, §6.4).
  u32 res_addr = 0;
  bool res_valid = false;
  alignas(64) u8 res_data[128];

  // Lifecycle.
  std::atomic<int> state{0};  // 0 init, 1 running, 2 stopped
  u32 exit_status = 0;
  bool thread_exited = false;  // stopped through stop 0x102 (exit status readable)
  std::atomic<bool> stop_request{false};
  void* host = nullptr;
  u64 steps = 0;
  std::atomic<u32> wait_ch{~0u};  // channel the thread is blocked on (diagnostics)
  // Diagnostics: sampled program counters and the most recent MFC commands.
  u32 pc_ring[64] = {};
  u32 pc_ring_pos = 0;
  struct MfcRec {
    u32 cmd, lsa, ea, size;
  } mfc_ring[16] = {};
  u32 mfc_ring_pos = 0;
  u64 mfc_count = 0, ch_reads = 0, ch_writes = 0, intr_events = 0, stops = 0;
  bool traced = false;  // --trace-spu matches this thread
  u64 op_hist[256] = {};  // executed instructions by decoded opcode (diagnostics)

  // Recompiled-code support: 128-byte LS lines holding validated compiled code. A DMA or store
  // into such a line bumps code_gen, which invalidates every cached validation (spu_interp.cpp).
  u64 code_lines[32] = {};
  u64 code_gen = 1, lines_gen = 1;
  struct CacheEnt {
    u64 gen;
    const void* fn;  // compiled chunk (spu_gen::Chunk*) or nullptr
  };
  std::unique_ptr<CacheEnt[]> dcache;  // one entry per LS word, allocated on first run
  u64 compiled_runs = 0, interp_blocks = 0;
};

// Marks a write to [lsa, lsa+size) of a thread's LS (DMA, PPU LS writes) for code invalidation.
inline void note_ls_write(Thread& t, u32 lsa, u32 size) {
  if (!size) return;
  u32 first = (lsa & (kLsSize - 1)) >> 7, last = ((lsa + size - 1) & (kLsSize - 1)) >> 7;
  if (last < first) last = (kLsSize >> 7) - 1;  // wrapped: be conservative
  for (u32 l = first; l <= last; ++l)
    if ((t.code_lines[l >> 6] >> (l & 63)) & 1) {
      ++t.code_gen;
      return;
    }
}

// Name of a decoded opcode (spu_interp.cpp), for diagnostics.
const char* op_name(u32 op);

// --trace-spu logging (bounded by --trace-spu-limit); cat: 1 MFC, 2 channels, 4 events/stops.
bool trace_on(Thread& t, u32 cat);

struct Group {
  u32 id = 0;
  std::string name;
  s32 prio = 0;
  u32 type = 0;
  u32 num = 0;
  u32 init_count = 0;
  std::vector<std::shared_ptr<Thread>> threads;  // indexed by spu_num
  std::mutex m;
  std::condition_variable cv;
  u32 run_state = GROUP_NOT_INITIALIZED;
  u32 running = 0;  // threads still running
  u32 join_cause = 0, exit_status = 0;
  bool stop_latched = false;  // stopped and not yet joined
  bool joining = false;
  bool terminating = false;
  // Event connections (sys_spu_thread_group_connect_event): RUN, EXCEPTION, SYSTEM_MODULE.
  u32 event_queue[3] = {};
};

// Executes the SPU program until it stops (runs on the thread's host thread).
void run_thread(Thread& t);
// Interpreter entry (spu_interp.cpp).
void interpret(Thread& t);

// Resets per-start state and deploys the image (§9.5). Called with the thread stopped.
void prepare_start(Thread& t);

// Channel helpers used by the interpreter.
u32 read_channel(Thread& t, u32 ch);
void write_channel(Thread& t, u32 ch, u32 v);
u32 channel_count(Thread& t, u32 ch);
// Interrupt delivery check (§3.14): if interrupts are enabled and an event is pending, disables
// interrupts, saves `next` in SRR0 and returns the vector (LS 0); otherwise returns `next`.
u32 check_interrupt(Thread& t, u32 next);
// True if a masked event is pending and unread (bisled condition).
bool event_pending(Thread& t);
void write_out_intr_mbox(Thread& t, u32 v);  // LV2 SPU event protocol (§7.5)
// stop/stopd: returns false if the thread must terminate.
bool stop_and_signal(Thread& t, u32 code);

// PPU-side operations (LV2 syscalls).
s32 write_in_mbox(Thread& t, u32 v);
s32 write_snr(Thread& t, u32 which, u32 v);
// Clears channel state that does not survive a thread stop (inbound mailbox).
void on_thread_stopped(Thread& t);

}  // namespace rt::spu
