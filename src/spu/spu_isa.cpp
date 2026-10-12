#include "spu/spu_isa.h"

#include <algorithm>
#include <set>

namespace spu {
namespace {

struct OpDef {
  Op op;
  u32 bits;  // opcode width
  u32 code;  // opcode value (top `bits` bits of the instruction word)
  const char* name;
};

#define D(n, b, c) {Op::n, b, c, #n}
const OpDef kOps[] = {
    D(SELB, 4, 0x8), D(SHUFB, 4, 0xB), D(MPYA, 4, 0xC), D(FNMS, 4, 0xD), D(FMA, 4, 0xE), D(FMS, 4, 0xF),
    D(ILA, 7, 0x21), D(HBRA, 7, 0x08), D(HBRR, 7, 0x09),
    D(MPYI, 8, 0x74), D(MPYUI, 8, 0x75), D(AI, 8, 0x1C), D(AHI, 8, 0x1D), D(SFI, 8, 0x0C), D(SFHI, 8, 0x0D),
    D(ANDI, 8, 0x14), D(ANDHI, 8, 0x15), D(ANDBI, 8, 0x16), D(ORI, 8, 0x04), D(ORHI, 8, 0x05), D(ORBI, 8, 0x06),
    D(XORI, 8, 0x44), D(XORHI, 8, 0x45), D(XORBI, 8, 0x46), D(CEQI, 8, 0x7C), D(CEQHI, 8, 0x7D),
    D(CEQBI, 8, 0x7E), D(CGTI, 8, 0x4C), D(CGTHI, 8, 0x4D), D(CGTBI, 8, 0x4E), D(CLGTI, 8, 0x5C),
    D(CLGTHI, 8, 0x5D), D(CLGTBI, 8, 0x5E), D(LQD, 8, 0x34), D(STQD, 8, 0x24), D(HEQI, 8, 0x7F),
    D(HGTI, 8, 0x4F), D(HLGTI, 8, 0x5F),
    D(LQA, 9, 0x061), D(LQR, 9, 0x067), D(STQA, 9, 0x041), D(STQR, 9, 0x047), D(ILH, 9, 0x083),
    D(ILHU, 9, 0x082), D(IL, 9, 0x081), D(IOHL, 9, 0x0C1), D(FSMBI, 9, 0x065), D(BR, 9, 0x064),
    D(BRA, 9, 0x060), D(BRSL, 9, 0x066), D(BRASL, 9, 0x062), D(BRNZ, 9, 0x042), D(BRZ, 9, 0x040),
    D(BRHNZ, 9, 0x046), D(BRHZ, 9, 0x044),
    D(CFLTS, 10, 0x1D8), D(CFLTU, 10, 0x1D9), D(CSFLT, 10, 0x1DA), D(CUFLT, 10, 0x1DB),
    D(LQX, 11, 0x1C4), D(STQX, 11, 0x144), D(CBX, 11, 0x1D4), D(CHX, 11, 0x1D5), D(CWX, 11, 0x1D6),
    D(CDX, 11, 0x1D7), D(CBD, 11, 0x1F4), D(CHD, 11, 0x1F5), D(CWD, 11, 0x1F6), D(CDD, 11, 0x1F7),
    D(AH, 11, 0x0C8), D(A, 11, 0x0C0), D(SFH, 11, 0x048), D(SF, 11, 0x040), D(ADDX, 11, 0x340),
    D(CG, 11, 0x0C2), D(CGX, 11, 0x342), D(SFX, 11, 0x341), D(BG, 11, 0x042), D(BGX, 11, 0x343),
    D(MPY, 11, 0x3C4), D(MPYU, 11, 0x3CC), D(MPYH, 11, 0x3C5), D(MPYS, 11, 0x3C7), D(MPYHH, 11, 0x3C6),
    D(MPYHHA, 11, 0x346), D(MPYHHU, 11, 0x3CE), D(MPYHHAU, 11, 0x34E), D(CLZ, 11, 0x2A5), D(CNTB, 11, 0x2B4),
    D(FSMB, 11, 0x1B6), D(FSMH, 11, 0x1B5), D(FSM, 11, 0x1B4), D(GBB, 11, 0x1B2), D(GBH, 11, 0x1B1),
    D(GB, 11, 0x1B0), D(AVGB, 11, 0x0D3), D(ABSDB, 11, 0x053), D(SUMB, 11, 0x253), D(XSBH, 11, 0x2B6),
    D(XSHW, 11, 0x2AE), D(XSWD, 11, 0x2A6), D(AND, 11, 0x0C1), D(ANDC, 11, 0x2C1), D(OR, 11, 0x041),
    D(ORC, 11, 0x2C9), D(ORX, 11, 0x1F0), D(XOR, 11, 0x241), D(NAND, 11, 0x0C9), D(NOR, 11, 0x049),
    D(EQV, 11, 0x249), D(SHLH, 11, 0x05F), D(SHLHI, 11, 0x07F), D(SHL, 11, 0x05B), D(SHLI, 11, 0x07B),
    D(SHLQBI, 11, 0x1DB), D(SHLQBII, 11, 0x1FB), D(SHLQBY, 11, 0x1DF), D(SHLQBYI, 11, 0x1FF),
    D(SHLQBYBI, 11, 0x1CF), D(ROTH, 11, 0x05C), D(ROTHI, 11, 0x07C), D(ROT, 11, 0x058), D(ROTI, 11, 0x078),
    D(ROTQBY, 11, 0x1DC), D(ROTQBYI, 11, 0x1FC), D(ROTQBYBI, 11, 0x1CC), D(ROTQBI, 11, 0x1D8),
    D(ROTQBII, 11, 0x1F8), D(ROTHM, 11, 0x05D), D(ROTHMI, 11, 0x07D), D(ROTM, 11, 0x059), D(ROTMI, 11, 0x079),
    D(ROTQMBY, 11, 0x1DD), D(ROTQMBYI, 11, 0x1FD), D(ROTQMBYBI, 11, 0x1CD), D(ROTQMBI, 11, 0x1D9),
    D(ROTQMBII, 11, 0x1F9), D(ROTMAH, 11, 0x05E), D(ROTMAHI, 11, 0x07E), D(ROTMA, 11, 0x05A),
    D(ROTMAI, 11, 0x07A), D(HEQ, 11, 0x3D8), D(HGT, 11, 0x258), D(HLGT, 11, 0x2D8), D(CEQB, 11, 0x3D0),
    D(CEQH, 11, 0x3C8), D(CEQ, 11, 0x3C0), D(CGTB, 11, 0x250), D(CGTH, 11, 0x248), D(CGT, 11, 0x240),
    D(CLGTB, 11, 0x2D0), D(CLGTH, 11, 0x2C8), D(CLGT, 11, 0x2C0), D(BI, 11, 0x1A8), D(IRET, 11, 0x1AA),
    D(BISLED, 11, 0x1AB), D(BISL, 11, 0x1A9), D(BIZ, 11, 0x128), D(BINZ, 11, 0x129), D(BIHZ, 11, 0x12A),
    D(BIHNZ, 11, 0x12B), D(HBR, 11, 0x1AC), D(FA, 11, 0x2C4), D(DFA, 11, 0x2CC), D(FS, 11, 0x2C5),
    D(DFS, 11, 0x2CD), D(FM, 11, 0x2C6), D(DFM, 11, 0x2CE), D(DFMA, 11, 0x35C), D(DFNMS, 11, 0x35E),
    D(DFMS, 11, 0x35D), D(DFNMA, 11, 0x35F), D(FREST, 11, 0x1B8), D(FRSQEST, 11, 0x1B9), D(FI, 11, 0x3D4),
    D(FRDS, 11, 0x3B9), D(FESD, 11, 0x3B8), D(DFCEQ, 11, 0x3C3), D(DFCMEQ, 11, 0x3CB), D(DFCGT, 11, 0x2C3),
    D(DFCMGT, 11, 0x2CB), D(DFTSV, 11, 0x3BF), D(FCEQ, 11, 0x3C2), D(FCMEQ, 11, 0x3CA), D(FCGT, 11, 0x2C2),
    D(FCMGT, 11, 0x2CA), D(FSCRWR, 11, 0x3BA), D(FSCRRD, 11, 0x398), D(STOP, 11, 0x000), D(STOPD, 11, 0x140),
    D(LNOP, 11, 0x001), D(NOP, 11, 0x201), D(SYNC, 11, 0x002), D(DSYNC, 11, 0x003), D(MFSPR, 11, 0x00C),
    D(MTSPR, 11, 0x10C), D(RDCH, 11, 0x00D), D(RCHCNT, 11, 0x00F), D(WRCH, 11, 0x10D),
};
#undef D

struct Tables {
  Op table[2048];
  const char* names[size_t(Op::COUNT)];
  Tables() {
    for (auto& e : table) e = Op::UNK;
    for (auto& n : names) n = "UNK";
    // Fill shortest opcodes first so longer (more specific) ones override where they match.
    for (u32 width = 4; width <= 11; ++width)
      for (const auto& d : kOps) {
        names[size_t(d.op)] = d.name;
        if (d.bits != width) continue;
        u32 shift = 11 - width;
        for (u32 low = 0; low < (1u << shift); ++low) table[(d.code << shift) | low] = d.op;
      }
  }
};
const Tables& tables() {
  static const Tables t;
  return t;
}

}  // namespace

Op decode(u32 w) { return tables().table[w >> 21]; }
const char* op_name(Op op) { return size_t(op) < size_t(Op::COUNT) ? tables().names[size_t(op)] : "?"; }

Flow flow(Op op) {
  switch (op) {
    case Op::BR: case Op::BRA: return Flow::Jump;
    case Op::BRZ: case Op::BRNZ: case Op::BRHZ: case Op::BRHNZ: return Flow::CondJump;
    case Op::BRSL: case Op::BRASL: return Flow::Call;
    case Op::BI: case Op::IRET: return Flow::Indirect;
    case Op::BIZ: case Op::BINZ: case Op::BIHZ: case Op::BIHNZ: return Flow::CondIndirect;
    case Op::BISL: case Op::BISLED: return Flow::IndirectCall;
    case Op::STOP: case Op::STOPD: return Flow::Stop;
    case Op::UNK: return Flow::Invalid;
    default: return Flow::Next;
  }
}

u32 branch_target(Op op, u32 w, u32 pc) {
  s32 s16 = s32(int16_t(u16((w >> 7) & 0xffff)));
  bool absolute = op == Op::BRA || op == Op::BRASL;
  return (absolute ? u32(s16 << 2) : pc + u32(s16 << 2)) & (kLsSize - 4);
}

Chunk discover_chunk(const u8* ls, u32 entry, u32 max_insns) {
  Chunk c;
  c.entry = entry & (kLsSize - 4);
  std::set<u32> seen, calls, rets;
  std::vector<u32> work{c.entry};
  while (!work.empty() && seen.size() < max_insns) {
    u32 pc = work.back();
    work.pop_back();
    while (pc < kLsSize && !seen.count(pc) && seen.size() < max_insns) {
      u32 w = ls_word(ls, pc);
      Op op = decode(w);
      Flow f = flow(op);
      // An all-zero word decodes as `stop 0`, which is never valid code (zero-filled LS).
      if (f == Flow::Invalid || w == 0) break;
      seen.insert(pc);
      bool cont = true;
      switch (f) {
        case Flow::Jump:
          work.push_back(branch_target(op, w, pc));
          cont = false;
          break;
        case Flow::CondJump:
          work.push_back(branch_target(op, w, pc));
          break;
        case Flow::Call:
          calls.insert(branch_target(op, w, pc));
          rets.insert(pc + 4);
          cont = false;
          break;
        case Flow::Indirect:
          cont = false;
          break;
        case Flow::IndirectCall:
          rets.insert(pc + 4);
          cont = false;
          break;
        default:  // Next, CondIndirect, Stop: fall through
          break;
      }
      if (!cont) break;
      pc += 4;
    }
  }
  c.insns = u32(seen.size());
  for (u32 pc : seen) {
    if (!c.ranges.empty() && c.ranges.back().second == pc) c.ranges.back().second = pc + 4;
    else c.ranges.push_back({pc, pc + 4});
  }
  c.call_targets.assign(calls.begin(), calls.end());
  for (u32 r : rets)
    if (r < kLsSize) c.return_points.push_back(r);
  return c;
}

std::vector<Chunk> discover_closure(const u8* ls, u32 entry, u32 max_chunks) {
  std::vector<Chunk> out;
  std::set<u32> done;
  std::vector<u32> work{entry & (kLsSize - 4)};
  while (!work.empty() && out.size() < max_chunks) {
    u32 e = work.back();
    work.pop_back();
    if (done.count(e)) continue;
    done.insert(e);
    Chunk c = discover_chunk(ls, e);
    if (!c.insns) continue;
    for (u32 t : c.call_targets) work.push_back(t);
    for (u32 r : c.return_points) work.push_back(r);
    out.push_back(std::move(c));
  }
  return out;
}

u64 chunk_hash(const u8* ls, const Chunk& c) {
  u64 h = 0xcbf29ce484222325ull;
  auto mix = [&](u32 v) {
    for (int i = 0; i < 4; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 0x100000001b3ull;
    }
  };
  mix(c.entry);
  for (auto [s, e] : c.ranges) {
    mix(s);
    mix(e);
    for (u32 a = s; a < e; a += 4) mix(ls_word(ls, a));
  }
  return h;
}

}  // namespace spu
