// SPU instruction set: opcode table, decoder and control-flow classification. Shared by the
// runtime (interpreter, dispatcher, code collection) and the offline SPU recompiler.
#pragma once
#include <vector>

#include "common/bytes.h"

namespace spu {

enum class Op : u16 {
  UNK,
  // RRR
  SELB, SHUFB, MPYA, FNMS, FMA, FMS,
  // RI18
  ILA, HBRA, HBRR,
  // RI10
  MPYI, MPYUI, AI, AHI, SFI, SFHI, ANDI, ANDHI, ANDBI, ORI, ORHI, ORBI, XORI, XORHI, XORBI, CEQI, CEQHI, CEQBI, CGTI,
  CGTHI, CGTBI, CLGTI, CLGTHI, CLGTBI, LQD, STQD, HEQI, HGTI, HLGTI,
  // RI16
  LQA, LQR, STQA, STQR, ILH, ILHU, IL, IOHL, FSMBI, BR, BRA, BRSL, BRASL, BRNZ, BRZ, BRHNZ, BRHZ,
  // RI8
  CFLTS, CFLTU, CSFLT, CUFLT,
  // RR / RI7
  LQX, STQX, CBX, CHX, CWX, CDX, CBD, CHD, CWD, CDD, AH, A, SFH, SF, ADDX, CG, CGX, SFX, BG, BGX, MPY, MPYU, MPYH, MPYS,
  MPYHH, MPYHHA, MPYHHU, MPYHHAU, CLZ, CNTB, FSMB, FSMH, FSM, GBB, GBH, GB, AVGB, ABSDB, SUMB, XSBH, XSHW, XSWD, AND, ANDC,
  OR, ORC, ORX, XOR, NAND, NOR, EQV, SHLH, SHLHI, SHL, SHLI, SHLQBI, SHLQBII, SHLQBY, SHLQBYI, SHLQBYBI, ROTH, ROTHI,
  ROT, ROTI, ROTQBY, ROTQBYI, ROTQBYBI, ROTQBI, ROTQBII, ROTHM, ROTHMI, ROTM, ROTMI, ROTQMBY, ROTQMBYI, ROTQMBYBI, ROTQMBI,
  ROTQMBII, ROTMAH, ROTMAHI, ROTMA, ROTMAI, HEQ, HGT, HLGT, CEQB, CEQH, CEQ, CGTB, CGTH, CGT, CLGTB, CLGTH, CLGT, BI, IRET,
  BISLED, BISL, BIZ, BINZ, BIHZ, BIHNZ, HBR, FA, DFA, FS, DFS, FM, DFM, DFMA, DFNMS, DFMS, DFNMA, FREST, FRSQEST, FI, FRDS,
  FESD, DFCEQ, DFCMEQ, DFCGT, DFCMGT, DFTSV, FCEQ, FCMEQ, FCGT, FCMGT, FSCRWR, FSCRRD, STOP, STOPD, LNOP, NOP, SYNC, DSYNC,
  MFSPR, MTSPR, RDCH, RCHCNT, WRCH,
  COUNT
};

// Decodes an instruction word (big-endian value).
Op decode(u32 w);
const char* op_name(Op op);

// Decoded fields (all formats; the opcode decides which are meaningful).
struct Fields {
  u32 rt, ra, rb, rrr_t, rrr_c;
  s32 i7, i10, s16;
  u32 i16, i18;
};
inline Fields fields(u32 w) {
  Fields f;
  f.rt = w & 0x7f;
  f.ra = (w >> 7) & 0x7f;
  f.rb = (w >> 14) & 0x7f;
  f.rrr_t = (w >> 21) & 0x7f;
  f.rrr_c = w & 0x7f;
  f.i7 = s32(w << 11) >> 25;
  f.i10 = s32(w << 8) >> 22;
  f.i16 = (w >> 7) & 0xffff;
  f.s16 = s32(int16_t(u16(f.i16)));
  f.i18 = (w >> 7) & 0x3ffff;
  return f;
}

// Control-flow classification used by chunk discovery.
enum class Flow : u8 {
  Next,         // falls through
  Jump,         // unconditional direct branch (br, bra)
  CondJump,     // conditional direct branch (brz, brnz, brhz, brhnz)
  Call,         // direct call (brsl, brasl): chunk ends; target and return point are entries
  Indirect,     // indirect branch (bi, iret): chunk ends
  CondIndirect, // conditional indirect branch (biz, binz, bihz, bihnz)
  IndirectCall, // bisl, bisled: chunk ends; return point is an entry
  Stop,         // stop/stopd: may resume at pc+4
  Invalid,      // undecodable word
};
Flow flow(Op op);
// Direct branch target (for Jump, CondJump, Call).
u32 branch_target(Op op, u32 w, u32 pc);

constexpr u32 kLsSize = 0x40000;

// A chunk: the instructions reachable from `entry` through fall-through and direct branches,
// without passing through calls or indirect branches. `ranges` are sorted, merged
// [start, end) byte ranges of the chunk's instructions.
struct Chunk {
  u32 entry = 0;
  std::vector<std::pair<u32, u32>> ranges;
  std::vector<u32> call_targets;    // direct call targets (new entries)
  std::vector<u32> return_points;   // pc+4 of calls (new entries)
  u32 insns = 0;
};
// Discovers the chunk at `entry` in the local-store image `ls` (big-endian words).
Chunk discover_chunk(const u8* ls, u32 entry, u32 max_insns = 8192);
// Discovers `entry` and every chunk reachable from it through calls and return points.
std::vector<Chunk> discover_closure(const u8* ls, u32 entry, u32 max_chunks = 4096);

// 64-bit FNV-1a over a chunk's entry, ranges and code bytes (its identity for validation).
u64 chunk_hash(const u8* ls, const Chunk& c);

inline u32 ls_word(const u8* ls, u32 a) {
  a &= kLsSize - 4;
  return (u32(ls[a]) << 24) | (u32(ls[a + 1]) << 16) | (u32(ls[a + 2]) << 8) | ls[a + 3];
}

}  // namespace spu
