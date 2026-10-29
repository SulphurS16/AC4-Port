// SPU execution: dispatcher for recompiled chunks (spu_gen) with the interpreter as fallback.
// Registers use the PPU VMX host layout (byte-reversed): the host little-endian 128-bit value
// equals the SPU's big-endian quadword and the preferred-slot word is host word 3.
//
// Recompiled chunks are validated against the current LS bytes before use; the validation is
// cached per LS word and invalidated when a DMA or store writes into a validated code line.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <unordered_map>

#include "common/file.h"
#include "ppu/ppu_recomp.h"
#include "runtime/config.h"
#include "runtime/log.h"
#include "runtime/spu/spu.h"
#include "spu/spu_gen.h"
#include "spu/spu_isa.h"
#include "spu/spu_ops.h"

namespace spu::ops {
[[noreturn]] void halt(rt::spu::Thread& t, const char* why, u32 pc, u32 w) {
  rt::fatal(ac4::strprintf("SPU %s halted (%s) at pc 0x%05x (insn 0x%08x)", t.name.c_str(), why, pc, w));
}
}  // namespace spu::ops

namespace rt::spu {

using ::spu::Op;
using ppu::v128;

const char* op_name(u32 op) { return ::spu::op_name(Op(op)); }

namespace {

// ---- recompiled chunk registry ----
struct Registry {
  std::unordered_map<u32, std::vector<const spu_gen::Chunk*>> by_entry;
  Registry() {
    for (u32 i = 0; i < spu_gen::g_chunk_count; ++i) by_entry[spu_gen::g_chunks[i].entry].push_back(&spu_gen::g_chunks[i]);
  }
};
const Registry& registry() {
  static const Registry r;
  return r;
}

bool matches(const Thread& t, const spu_gen::Chunk& c) {
  const u8* code = reinterpret_cast<const u8*>(c.code);
  for (u32 i = 0; i < c.nranges; ++i) {
    u32 s = c.ranges[2 * i], e = c.ranges[2 * i + 1];
    if (std::memcmp(t.ls + s, code, e - s) != 0) return false;
    code += e - s;
  }
  return true;
}

void mark_code(Thread& t, const spu_gen::Chunk& c) {
  for (u32 i = 0; i < c.nranges; ++i)
    for (u32 l = c.ranges[2 * i] >> 7; l <= (c.ranges[2 * i + 1] - 1) >> 7; ++l) t.code_lines[l >> 6] |= 1ull << (l & 63);
}

// ---- code collection (--spu-collect): records chunk closures that have no compiled version ----
std::mutex g_collect_mu;
std::set<u64> g_collected;

void collect(Thread& t, u32 pc) {
  std::vector<::spu::Chunk> chunks = ::spu::discover_closure(t.ls, pc);
  if (chunks.empty()) return;
  u64 h = ::spu::chunk_hash(t.ls, chunks[0]);
  {
    std::lock_guard lk(g_collect_mu);
    if (!g_collected.insert(h).second) return;
  }
  // Record: header, entries, merged ranges of the whole closure, bytes.
  std::set<u32> entries;
  std::vector<std::pair<u32, u32>> lines;
  for (const auto& c : chunks) {
    entries.insert(c.entry);
    for (auto r : c.ranges) lines.push_back(r);
  }
  std::sort(lines.begin(), lines.end());
  std::vector<std::pair<u32, u32>> merged;
  for (auto r : lines) {
    if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
    else merged.push_back(r);
  }
  std::vector<u8> out;
  auto put32 = [&](u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(u8(v >> (8 * i)));
  };
  out.insert(out.end(), {'S', 'P', 'U', 'R', 'E', 'C', '1', 0});
  put32(u32(entries.size()));
  put32(u32(merged.size()));
  for (u32 e : entries) put32(e);
  for (auto [s, e] : merged) {
    put32(s);
    put32(e);
  }
  for (auto [s, e] : merged) out.insert(out.end(), t.ls + s, t.ls + e);
  std::filesystem::path dir = std::filesystem::path(cfg().spu_cache);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string name = ac4::strprintf("%016llx.spurec", (unsigned long long)h);
  ac4::write_file((dir / name).string(), out);
  LOG_I("spu", "collected SPU code at 0x%05x (%zu chunks, %zu ranges) from '%s' -> %s", pc, chunks.size(), merged.size(),
        t.name.c_str(), name.c_str());
}

// Code without a compiled version runs in the interpreter, many times slower (enough to upset
// the game's timing): said once per SPU thread name so that missing AOT coverage is visible.
void note_interpreted(const Thread& t, u32 pc) {
  static std::mutex mu;
  static std::set<std::string> seen;
  std::lock_guard lk(mu);
  if (!seen.insert(t.name).second) return;
  LOG_W("spu", "SPU '%s' runs code without a compiled version (from 0x%05x): interpreted, much slower; record it with "
        "--spu-collect and compile it with tools/spu_aot.ps1", t.name.c_str(), pc);
}

const spu_gen::Chunk* lookup(Thread& t, u32 pc) {
  Thread::CacheEnt& e = t.dcache[pc >> 2];
  if (e.gen == t.code_gen) return static_cast<const spu_gen::Chunk*>(e.fn);
  if (t.lines_gen != t.code_gen) {  // validations are stale: start marking afresh
    std::memset(t.code_lines, 0, sizeof(t.code_lines));
    t.lines_gen = t.code_gen;
  }
  const spu_gen::Chunk* found = nullptr;
  auto it = registry().by_entry.find(pc);
  if (it != registry().by_entry.end() && !cfg().spu_interp)
    for (const spu_gen::Chunk* c : it->second)
      if (matches(t, *c)) {
        found = c;
        mark_code(t, *c);
        break;
      }
  if (!found && cfg().spu_collect) collect(t, pc);
  else if (!found && !cfg().spu_interp) note_interpreted(t, pc);
  e.gen = t.code_gen;
  e.fn = found;
  return found;
}

// The instruction at pc branches to itself (br . / bra pc): an SPU waiting for an interrupt.
bool self_branch(const Thread& t, u32 pc) {
  const u32 w = __builtin_bswap32(*reinterpret_cast<const u32*>(t.ls + (pc & 0x3fffc)));
  const u32 op9 = w >> 23, i16 = (w >> 7) & 0xffff;
  if (op9 == 0x064) return i16 == 0;                               // br: pc-relative
  if (op9 == 0x060) return (u32(s32(int16_t(i16)) << 2) & 0x3fffc) == (pc & 0x3fffc);  // bra: absolute
  return false;
}

// ---- interpreter ----
// Executes from t.pc until control leaves straight-line flow (taken branch, call, indirect
// branch, resumed stop) or `budget` instructions have run. Returns false if the thread stops.
bool interpret_block(Thread& t, u32 budget) {
  v128* g = t.gpr;
  for (u32 n = 0; n < budget; ++n) {
    if (t.stop_request.load(std::memory_order_relaxed)) return false;
    const u32 pc = t.pc;
    const u32 w = __builtin_bswap32(*reinterpret_cast<const u32*>(t.ls + (pc & 0x3fffc)));
    const Op op = ::spu::decode(w);
    const ::spu::Fields f = ::spu::fields(w);
    const u32 rt = f.rt, ra = f.ra, rb = f.rb, rrr_t = f.rrr_t, rrr_c = f.rrr_c, i16 = f.i16, i18 = f.i18;
    const s32 i7 = f.i7, i10 = f.i10, s16 = f.s16;
    (void)i16, (void)i18, (void)i7, (void)i10, (void)s16, (void)rrr_t, (void)rrr_c;
    u32 next = pc + 4;
    if ((++t.steps & 0xFFFF) == 0) t.pc_ring[t.pc_ring_pos++ & 63] = pc;
    ++t.op_hist[u32(op) & 255];
    switch (op) {
#define SPU_CASE(name) \
  case Op::name:       \
    SPU_OP_##name;     \
    break;
      SPU_SIMPLE_OPS(SPU_CASE)
#undef SPU_CASE
      case Op::BR: next = pc + u32(s16 << 2); break;
      case Op::BRA: next = u32(s16 << 2); break;
      case Op::BRSL:
        g[rt] = ::spu::ops::scalar(pc + 4);
        next = pc + u32(s16 << 2);
        break;
      case Op::BRASL:
        g[rt] = ::spu::ops::scalar(pc + 4);
        next = u32(s16 << 2);
        break;
      case Op::BRZ: if (SPU_PREF(rt) == 0) next = pc + u32(s16 << 2); break;
      case Op::BRNZ: if (SPU_PREF(rt) != 0) next = pc + u32(s16 << 2); break;
      case Op::BRHZ: if ((SPU_PREF(rt) & 0xffff) == 0) next = pc + u32(s16 << 2); break;
      case Op::BRHNZ: if ((SPU_PREF(rt) & 0xffff) != 0) next = pc + u32(s16 << 2); break;
      case Op::BI:
      case Op::BISL:
      case Op::BISLED:
      case Op::BIZ:
      case Op::BINZ:
      case Op::BIHZ:
      case Op::BIHNZ:
      case Op::IRET: {
        bool take = true;
        if (op == Op::BISLED) take = event_pending(t);  // branches only if an event is pending
        if (op == Op::BIZ) take = SPU_PREF(rt) == 0;
        if (op == Op::BINZ) take = SPU_PREF(rt) != 0;
        if (op == Op::BIHZ) take = (SPU_PREF(rt) & 0xffff) == 0;
        if (op == Op::BIHNZ) take = (SPU_PREF(rt) & 0xffff) != 0;
        u32 target = op == Op::IRET ? t.srr0 : SPU_PREF(ra);
        if (op == Op::BISL || op == Op::BISLED) g[rt] = ::spu::ops::scalar(pc + 4);
        u32 de = (w >> 18) & 3;  // E (bit 13) enables, D (bit 12) disables interrupts
        if (de == 1) t.int_enabled = true;
        if (de == 2) t.int_enabled = false;
        if (take) next = target & 0x3fffc;
        if (de == 1) next = check_interrupt(t, next);  // takes effect after the branch
        break;
      }
      case Op::RDCH:
        t.pc = pc;
        g[rt] = ::spu::ops::scalar(read_channel(t, ra));
        break;
      case Op::RCHCNT:
        t.pc = pc;
        g[rt] = ::spu::ops::scalar(channel_count(t, ra));
        break;
      case Op::WRCH:
        t.pc = pc;
        write_channel(t, ra, SPU_PREF(rt));
        if (t.int_enabled) next = check_interrupt(t, next);
        break;
      case Op::STOP:
      case Op::STOPD:
        t.pc = pc;
        if (!stop_and_signal(t, w & 0x3fff)) return false;
        next = t.pc;  // stop_and_signal sets the resume pc
        break;
      case Op::UNK:
      default:
        ::spu::ops::halt(t, "unknown instruction", pc, w);
    }
    t.pc = next & 0x3fffc;
    if (next != pc + 4) return true;
  }
  return true;
}

}  // namespace

void interpret(Thread& t) {
  // SPU single precision has no denormals and rounds toward zero: FTZ + DAZ + RZ on this host
  // thread (spu_facts.md §3.8). Recompiled code runs on the same thread.
  _mm_setcsr(_mm_getcsr() | 0xE040);
  if (!t.dcache) t.dcache.reset(new Thread::CacheEnt[kLsSize / 4]());
  const auto parking = [] { return true; };
  for (;;) {
    if (t.stop_request.load(std::memory_order_relaxed)) return;
    if (t.int_enabled) {
      t.pc = check_interrupt(t, t.pc);
      // A branch to itself with interrupts enabled waits for the next event (SPURS kernels idle
      // this way): block until one is pending instead of looping on a host core (on two logical
      // processors these loops took most of the processor time from the PPU threads that give
      // them work; run 20261024-170850).
      if (t.int_enabled && parking() && self_branch(t, t.pc)) {
        idle_until_event(t);
        continue;
      }
    } else if (parking() && self_branch(t, t.pc)) {  // interrupts disabled: only a stop ends it
      park_self_branch(t);
      continue;
    }
    const spu_gen::Chunk* c = lookup(t, t.pc);
    if (c) {
      ++t.compiled_runs;
      if (!c->fn(t)) return;
    } else {
      ++t.interp_blocks;
      if (!interpret_block(t, 4096)) return;
    }
  }
}

}  // namespace rt::spu
