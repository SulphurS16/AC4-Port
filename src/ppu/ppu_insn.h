// PPU (Cell PowerPC 64 + VMX) instruction decoding shared by the recompiler, the disassembler
// and the interpreter. Field names follow the Power ISA (bit 0 = MSB of the 32-bit word).
#pragma once
#include <cstdint>

#include "common/bytes.h"

namespace ppu {

#define PPU_OPS(X)                                                                                         \
  X(INVALID) X(TDI) X(TWI) X(MULLI) X(SUBFIC) X(CMPLI) X(CMPI) X(ADDIC) X(ADDIC_) X(ADDI) X(ADDIS) X(BC)    \
  X(SC) X(B) X(MCRF) X(BCLR) X(CRNOR) X(CRANDC) X(ISYNC) X(CRXOR) X(CRNAND) X(CRAND) X(CREQV) X(CRORC)      \
  X(CROR) X(BCCTR) X(RLWIMI) X(RLWINM) X(RLWNM) X(ORI) X(ORIS) X(XORI) X(XORIS) X(ANDI_) X(ANDIS_)          \
  X(RLDICL) X(RLDICR) X(RLDIC) X(RLDIMI) X(RLDCL) X(RLDCR)                                                 \
  /* opcode 31 */                                                                                          \
  X(CMP) X(TW) X(LVSL) X(LVEBX) X(SUBFC) X(MULHDU) X(ADDC) X(MULHWU) X(MFCR) X(MFOCRF) X(LWARX) X(LDX)      \
  X(LWZX) X(SLW) X(CNTLZW) X(SLD) X(AND) X(CMPL) X(LVSR) X(LVEHX) X(SUBF) X(LDUX) X(DCBST) X(LWZUX)         \
  X(CNTLZD) X(ANDC) X(TD) X(LVEWX) X(MULHD) X(MULHW) X(MFMSR) X(LDARX) X(DCBF) X(LBZX) X(LVX) X(NEG)       \
  X(LBZUX) X(NOR) X(STVEBX) X(SUBFE) X(ADDE) X(MTCRF) X(MTOCRF) X(MTMSR) X(STDX) X(STWCX) X(STWX)          \
  X(STVEHX) X(MTMSRD) X(STDUX) X(STWUX) X(STVEWX) X(SUBFZE) X(ADDZE) X(STDCX) X(STBX) X(STVX) X(SUBFME)    \
  X(MULLD) X(ADDME) X(MULLW) X(DCBTST) X(STBUX) X(ADD) X(DCBT) X(LHZX) X(EQV) X(ECIWX) X(LHZUX) X(XOR)     \
  X(MFSPR) X(LWAX) X(DST) X(LHAX) X(LVXL) X(MFTB) X(LWAUX) X(DSTST) X(LHAUX) X(STHX) X(ORC) X(ECOWX)       \
  X(STHUX) X(OR) X(DIVDU) X(DIVWU) X(MTSPR) X(DCBI) X(NAND) X(STVXL) X(DIVD) X(DIVW) X(LVLX) X(LDBRX)      \
  X(LSWX) X(LWBRX) X(LFSX) X(SRW) X(SRD) X(LVRX) X(LFSUX) X(LSWI) X(SYNC) X(LFDX) X(LFDUX) X(STVLX)       \
  X(STDBRX) X(STSWX) X(STWBRX) X(STFSX) X(STVRX) X(STFSUX) X(STSWI) X(STFDX) X(STFDUX) X(LVLXL) X(LHBRX)   \
  X(SRAW) X(SRAD) X(LVRXL) X(DSS) X(SRAWI) X(SRADI) X(EIEIO) X(STVLXL) X(STHBRX) X(EXTSH) X(STVRXL)       \
  X(EXTSB) X(STFIWX) X(EXTSW) X(ICBI) X(DCBZ)                                                              \
  /* loads/stores */                                                                                       \
  X(LWZ) X(LWZU) X(LBZ) X(LBZU) X(STW) X(STWU) X(STB) X(STBU) X(LHZ) X(LHZU) X(LHA) X(LHAU) X(STH) X(STHU) \
  X(LMW) X(STMW) X(LFS) X(LFSU) X(LFD) X(LFDU) X(STFS) X(STFSU) X(STFD) X(STFDU) X(LD) X(LDU) X(LWA)       \
  X(STD) X(STDU)                                                                                           \
  /* FPU */                                                                                                \
  X(FDIVS) X(FSUBS) X(FADDS) X(FSQRTS) X(FRES) X(FMULS) X(FMSUBS) X(FMADDS) X(FNMSUBS) X(FNMADDS)         \
  X(FCMPU) X(FRSP) X(FCTIW) X(FCTIWZ) X(FDIV) X(FSUB) X(FADD) X(FSQRT) X(FSEL) X(FMUL) X(FRSQRTE)          \
  X(FMSUB) X(FMADD) X(FNMSUB) X(FNMADD) X(FCMPO) X(MTFSB1) X(FNEG) X(MCRFS) X(MTFSB0) X(FMR) X(MTFSFI)    \
  X(FNABS) X(FABS) X(MFFS) X(MTFSF) X(FCTID) X(FCTIDZ) X(FCFID)                                            \
  /* VMX VA-form */                                                                                        \
  X(VMHADDSHS) X(VMHRADDSHS) X(VMLADDUHM) X(VMSUMUBM) X(VMSUMMBM) X(VMSUMUHM) X(VMSUMUHS) X(VMSUMSHM)      \
  X(VMSUMSHS) X(VSEL) X(VPERM) X(VSLDOI) X(VMADDFP) X(VNMSUBFP)                                            \
  /* VMX VX-form */                                                                                        \
  X(VADDUBM) X(VMAXUB) X(VRLB) X(VMULOUB) X(VADDFP) X(VMRGHB) X(VPKUHUM) X(VADDUHM) X(VMAXUH) X(VRLH)      \
  X(VMULOUH) X(VSUBFP) X(VMRGHH) X(VPKUWUM) X(VADDUWM) X(VMAXUW) X(VRLW) X(VMRGHW) X(VPKUHUS) X(VPKUWUS)  \
  X(VMAXSB) X(VSLB) X(VMULOSB) X(VREFP) X(VMRGLB) X(VPKSHUS) X(VMAXSH) X(VSLH) X(VMULOSH) X(VRSQRTEFP)     \
  X(VMRGLH) X(VPKSWUS) X(VADDCUW) X(VMAXSW) X(VSLW) X(VEXPTEFP) X(VMRGLW) X(VPKSHSS) X(VSL) X(VLOGEFP)    \
  X(VPKSWSS) X(VADDUBS) X(VMINUB) X(VSRB) X(VMULEUB) X(VRFIN) X(VSPLTB) X(VUPKHSB) X(VADDUHS) X(VMINUH)   \
  X(VSRH) X(VMULEUH) X(VRFIZ) X(VSPLTH) X(VUPKHSH) X(VADDUWS) X(VMINUW) X(VSRW) X(VRFIP) X(VSPLTW)        \
  X(VUPKLSB) X(VSR) X(VRFIM) X(VUPKLSH) X(VADDSBS) X(VMINSB) X(VSRAB) X(VMULESB) X(VCFUX) X(VSPLTISB)     \
  X(VPKPX) X(VADDSHS) X(VMINSH) X(VSRAH) X(VMULESH) X(VCFSX) X(VSPLTISH) X(VUPKHPX) X(VADDSWS) X(VMINSW)  \
  X(VSRAW) X(VCTUXS) X(VSPLTISW) X(VCTSXS) X(VUPKLPX) X(VSUBUBM) X(VAVGUB) X(VAND) X(VMAXFP) X(VSLO)      \
  X(VSUBUHM) X(VAVGUH) X(VANDC) X(VMINFP) X(VSRO) X(VSUBUWM) X(VAVGUW) X(VOR) X(VXOR) X(VAVGSB) X(VNOR)   \
  X(VAVGSH) X(VSUBCUW) X(VAVGSW) X(VSUBUBS) X(MFVSCR) X(VSUM4UBS) X(VSUBUHS) X(MTVSCR) X(VSUM4SHS)       \
  X(VSUBUWS) X(VSUM2SWS) X(VSUBSBS) X(VSUM4SBS) X(VSUBSHS) X(VSUBSWS) X(VSUMSWS)                          \
  /* VMX VC-form (compares, optional Rc) */                                                                \
  X(VCMPEQUB) X(VCMPEQUH) X(VCMPEQUW) X(VCMPEQFP) X(VCMPGEFP) X(VCMPGTUB) X(VCMPGTUH) X(VCMPGTUW)         \
  X(VCMPGTFP) X(VCMPGTSB) X(VCMPGTSH) X(VCMPGTSW) X(VCMPBFP)

enum class Op : u16 {
#define PPU_ENUM(n) n,
  PPU_OPS(PPU_ENUM)
#undef PPU_ENUM
      COUNT
};

const char* op_name(Op op);
Op decode(u32 w);

// ---- field extraction ----
inline u32 opcd(u32 w) { return w >> 26; }
inline u32 rd(u32 w) { return (w >> 21) & 31; }  // also RS, TO, BO, FRT, FRS, VD, VS
inline u32 ra(u32 w) { return (w >> 16) & 31; }  // also BI, FRA, VA
inline u32 rb(u32 w) { return (w >> 11) & 31; }  // also FRB, VB
inline u32 rc_(u32 w) { return (w >> 6) & 31; }  // FRC (A-form), VC (VA-form), MB (M-form)
inline u32 me(u32 w) { return (w >> 1) & 31; }
inline bool rc(u32 w) { return w & 1; }
inline bool oe(u32 w) { return (w >> 10) & 1; }
inline bool lk(u32 w) { return w & 1; }
inline bool aa(u32 w) { return (w >> 1) & 1; }
inline s32 simm(u32 w) { return s16(w & 0xffff); }
inline u32 uimm(u32 w) { return w & 0xffff; }
inline s32 bd(u32 w) { return s16(w & 0xfffc); }
inline s32 li(u32 w) { return s32((w & 0x03fffffc) << 6) >> 6; }
inline s32 ds(u32 w) { return s16(w & 0xfffc); }
inline u32 crfd(u32 w) { return (w >> 23) & 7; }
inline u32 crfs(u32 w) { return (w >> 18) & 7; }
inline u32 cmp_l(u32 w) { return (w >> 21) & 1; }
inline u32 md_sh(u32 w) { return ((w >> 11) & 31) | (((w >> 1) & 1) << 5); }
inline u32 md_mb(u32 w) { return ((w >> 6) & 31) | (((w >> 5) & 1) << 5); }  // also ME for rldicr/rldcr
inline u32 spr(u32 w) { return ((w >> 16) & 31) | (((w >> 11) & 31) << 5); }
inline u32 crm(u32 w) { return (w >> 12) & 0xff; }
inline u32 fm(u32 w) { return (w >> 17) & 0xff; }
inline u32 vsh(u32 w) { return (w >> 6) & 15; }                   // vsldoi
inline u32 vuimm(u32 w) { return (w >> 16) & 31; }                // vsplt*, vcf*, vct*
inline s32 vsimm(u32 w) { return s32((w >> 16) << 27) >> 27; }    // vspltis*
inline bool vrc(u32 w) { return (w >> 10) & 1; }                  // VC-form record bit

// Rotate mask with IBM bit numbering (bit 0 = MSB), inclusive, wrapping when mb > me.
constexpr u64 mask64(u32 mb, u32 me) {
  u64 m1 = ~0ull >> mb, m2 = ~0ull << (63 - me);
  return mb <= me ? (m1 & m2) : (m1 | m2);
}

// Branch helpers
inline bool bo_no_ctr(u32 bo) { return bo & 4; }  // BO[2]: don't decrement CTR
inline bool bo_no_cond(u32 bo) { return bo & 16; }  // BO[0]: ignore CR bit

// Classification used by analysis.
inline bool is_branch(Op op) { return op == Op::B || op == Op::BC || op == Op::BCLR || op == Op::BCCTR; }

// Disassembles one instruction (best effort, for logs and listings).
void disasm(u32 addr, u32 w, char* out, size_t outlen);

}  // namespace ppu
