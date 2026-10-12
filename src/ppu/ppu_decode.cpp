#include <cstdio>

#include "ppu/ppu_insn.h"

namespace ppu {

const char* op_name(Op op) {
  static const char* names[] = {
#define PPU_NAME(n) #n,
      PPU_OPS(PPU_NAME)
#undef PPU_NAME
  };
  u16 i = u16(op);
  return i < u16(Op::COUNT) ? names[i] : "?";
}

static Op decode19(u32 w) {
  switch ((w >> 1) & 0x3ff) {
    case 0: return Op::MCRF;
    case 16: return Op::BCLR;
    case 33: return Op::CRNOR;
    case 129: return Op::CRANDC;
    case 150: return Op::ISYNC;
    case 193: return Op::CRXOR;
    case 225: return Op::CRNAND;
    case 257: return Op::CRAND;
    case 289: return Op::CREQV;
    case 417: return Op::CRORC;
    case 449: return Op::CROR;
    case 528: return Op::BCCTR;
  }
  return Op::INVALID;
}

static Op decode30(u32 w) {
  switch ((w >> 2) & 7) {
    case 0: return Op::RLDICL;
    case 1: return Op::RLDICR;
    case 2: return Op::RLDIC;
    case 3: return Op::RLDIMI;
  }
  switch ((w >> 1) & 15) {
    case 8: return Op::RLDCL;
    case 9: return Op::RLDCR;
  }
  return Op::INVALID;
}

static Op decode31(u32 w) {
  u32 xo = (w >> 1) & 0x3ff;
  // XO-form arithmetic: OE is the top bit of the 10-bit field.
  switch (xo & 0x1ff) {
    case 8: return Op::SUBFC;
    case 10: return Op::ADDC;
    case 40: return Op::SUBF;
    case 104: return Op::NEG;
    case 136: return Op::SUBFE;
    case 138: return Op::ADDE;
    case 200: return Op::SUBFZE;
    case 202: return Op::ADDZE;
    case 232: return Op::SUBFME;
    case 233: return Op::MULLD;
    case 234: return Op::ADDME;
    case 235: return Op::MULLW;
    case 266: return Op::ADD;
    case 457: return Op::DIVDU;
    case 459: return Op::DIVWU;
    case 489: return Op::DIVD;
    case 491: return Op::DIVW;
    default: break;
  }
  switch (xo) {
    case 9: return Op::MULHDU;
    case 11: return Op::MULHWU;
    case 73: return Op::MULHD;
    case 75: return Op::MULHW;
    case 0: return Op::CMP;
    case 4: return Op::TW;
    case 6: return Op::LVSL;
    case 7: return Op::LVEBX;
    case 19: return (w >> 20) & 1 ? Op::MFOCRF : Op::MFCR;
    case 20: return Op::LWARX;
    case 21: return Op::LDX;
    case 23: return Op::LWZX;
    case 24: return Op::SLW;
    case 26: return Op::CNTLZW;
    case 27: return Op::SLD;
    case 28: return Op::AND;
    case 32: return Op::CMPL;
    case 38: return Op::LVSR;
    case 39: return Op::LVEHX;
    case 53: return Op::LDUX;
    case 54: return Op::DCBST;
    case 55: return Op::LWZUX;
    case 58: return Op::CNTLZD;
    case 60: return Op::ANDC;
    case 68: return Op::TD;
    case 71: return Op::LVEWX;
    case 83: return Op::MFMSR;
    case 84: return Op::LDARX;
    case 86: return Op::DCBF;
    case 87: return Op::LBZX;
    case 103: return Op::LVX;
    case 119: return Op::LBZUX;
    case 124: return Op::NOR;
    case 135: return Op::STVEBX;
    case 144: return (w >> 20) & 1 ? Op::MTOCRF : Op::MTCRF;
    case 146: return Op::MTMSR;
    case 149: return Op::STDX;
    case 150: return Op::STWCX;
    case 151: return Op::STWX;
    case 167: return Op::STVEHX;
    case 178: return Op::MTMSRD;
    case 181: return Op::STDUX;
    case 183: return Op::STWUX;
    case 199: return Op::STVEWX;
    case 214: return Op::STDCX;
    case 215: return Op::STBX;
    case 231: return Op::STVX;
    case 246: return Op::DCBTST;
    case 247: return Op::STBUX;
    case 278: return Op::DCBT;
    case 279: return Op::LHZX;
    case 284: return Op::EQV;
    case 310: return Op::ECIWX;
    case 311: return Op::LHZUX;
    case 316: return Op::XOR;
    case 339: return Op::MFSPR;
    case 341: return Op::LWAX;
    case 342: return Op::DST;
    case 343: return Op::LHAX;
    case 359: return Op::LVXL;
    case 371: return Op::MFTB;
    case 373: return Op::LWAUX;
    case 374: return Op::DSTST;
    case 375: return Op::LHAUX;
    case 407: return Op::STHX;
    case 412: return Op::ORC;
    case 438: return Op::ECOWX;
    case 439: return Op::STHUX;
    case 444: return Op::OR;
    case 467: return Op::MTSPR;
    case 470: return Op::DCBI;
    case 476: return Op::NAND;
    case 487: return Op::STVXL;
    case 519: return Op::LVLX;
    case 532: return Op::LDBRX;
    case 533: return Op::LSWX;
    case 534: return Op::LWBRX;
    case 535: return Op::LFSX;
    case 536: return Op::SRW;
    case 539: return Op::SRD;
    case 551: return Op::LVRX;
    case 567: return Op::LFSUX;
    case 597: return Op::LSWI;
    case 598: return Op::SYNC;
    case 599: return Op::LFDX;
    case 631: return Op::LFDUX;
    case 647: return Op::STVLX;
    case 660: return Op::STDBRX;
    case 661: return Op::STSWX;
    case 662: return Op::STWBRX;
    case 663: return Op::STFSX;
    case 679: return Op::STVRX;
    case 695: return Op::STFSUX;
    case 725: return Op::STSWI;
    case 727: return Op::STFDX;
    case 759: return Op::STFDUX;
    case 775: return Op::LVLXL;
    case 790: return Op::LHBRX;
    case 792: return Op::SRAW;
    case 794: return Op::SRAD;
    case 807: return Op::LVRXL;
    case 822: return Op::DSS;
    case 824: return Op::SRAWI;
    case 826:
    case 827: return Op::SRADI;
    case 854: return Op::EIEIO;
    case 903: return Op::STVLXL;
    case 918: return Op::STHBRX;
    case 922: return Op::EXTSH;
    case 935: return Op::STVRXL;
    case 954: return Op::EXTSB;
    case 982: return Op::ICBI;
    case 983: return Op::STFIWX;
    case 986: return Op::EXTSW;
    case 1014: return Op::DCBZ;
  }
  return Op::INVALID;
}

static Op decode59(u32 w) {
  switch ((w >> 1) & 31) {
    case 18: return Op::FDIVS;
    case 20: return Op::FSUBS;
    case 21: return Op::FADDS;
    case 22: return Op::FSQRTS;
    case 24: return Op::FRES;
    case 25: return Op::FMULS;
    case 28: return Op::FMSUBS;
    case 29: return Op::FMADDS;
    case 30: return Op::FNMSUBS;
    case 31: return Op::FNMADDS;
  }
  return Op::INVALID;
}

static Op decode63(u32 w) {
  switch ((w >> 1) & 31) {
    case 18: return Op::FDIV;
    case 20: return Op::FSUB;
    case 21: return Op::FADD;
    case 22: return Op::FSQRT;
    case 23: return Op::FSEL;
    case 25: return Op::FMUL;
    case 26: return Op::FRSQRTE;
    case 28: return Op::FMSUB;
    case 29: return Op::FMADD;
    case 30: return Op::FNMSUB;
    case 31: return Op::FNMADD;
  }
  switch ((w >> 1) & 0x3ff) {
    case 0: return Op::FCMPU;
    case 12: return Op::FRSP;
    case 14: return Op::FCTIW;
    case 15: return Op::FCTIWZ;
    case 32: return Op::FCMPO;
    case 38: return Op::MTFSB1;
    case 40: return Op::FNEG;
    case 64: return Op::MCRFS;
    case 70: return Op::MTFSB0;
    case 72: return Op::FMR;
    case 134: return Op::MTFSFI;
    case 136: return Op::FNABS;
    case 264: return Op::FABS;
    case 583: return Op::MFFS;
    case 711: return Op::MTFSF;
    case 814: return Op::FCTID;
    case 815: return Op::FCTIDZ;
    case 846: return Op::FCFID;
  }
  return Op::INVALID;
}

static Op decode4(u32 w) {
  u32 x6 = w & 0x3f;
  if (x6 >= 32) {
    switch (x6) {
      case 32: return Op::VMHADDSHS;
      case 33: return Op::VMHRADDSHS;
      case 34: return Op::VMLADDUHM;
      case 36: return Op::VMSUMUBM;
      case 37: return Op::VMSUMMBM;
      case 38: return Op::VMSUMUHM;
      case 39: return Op::VMSUMUHS;
      case 40: return Op::VMSUMSHM;
      case 41: return Op::VMSUMSHS;
      case 42: return Op::VSEL;
      case 43: return Op::VPERM;
      case 44: return Op::VSLDOI;
      case 46: return Op::VMADDFP;
      case 47: return Op::VNMSUBFP;
    }
    return Op::INVALID;
  }
  if (x6 == 6) {
    switch (w & 0x3ff) {
      case 6: return Op::VCMPEQUB;
      case 70: return Op::VCMPEQUH;
      case 134: return Op::VCMPEQUW;
      case 198: return Op::VCMPEQFP;
      case 454: return Op::VCMPGEFP;
      case 518: return Op::VCMPGTUB;
      case 582: return Op::VCMPGTUH;
      case 646: return Op::VCMPGTUW;
      case 710: return Op::VCMPGTFP;
      case 774: return Op::VCMPGTSB;
      case 838: return Op::VCMPGTSH;
      case 902: return Op::VCMPGTSW;
      case 966: return Op::VCMPBFP;
    }
    return Op::INVALID;
  }
  switch (w & 0x7ff) {
    case 0: return Op::VADDUBM;
    case 2: return Op::VMAXUB;
    case 4: return Op::VRLB;
    case 8: return Op::VMULOUB;
    case 10: return Op::VADDFP;
    case 12: return Op::VMRGHB;
    case 14: return Op::VPKUHUM;
    case 64: return Op::VADDUHM;
    case 66: return Op::VMAXUH;
    case 68: return Op::VRLH;
    case 72: return Op::VMULOUH;
    case 74: return Op::VSUBFP;
    case 76: return Op::VMRGHH;
    case 78: return Op::VPKUWUM;
    case 128: return Op::VADDUWM;
    case 130: return Op::VMAXUW;
    case 132: return Op::VRLW;
    case 140: return Op::VMRGHW;
    case 142: return Op::VPKUHUS;
    case 206: return Op::VPKUWUS;
    case 258: return Op::VMAXSB;
    case 260: return Op::VSLB;
    case 264: return Op::VMULOSB;
    case 266: return Op::VREFP;
    case 268: return Op::VMRGLB;
    case 270: return Op::VPKSHUS;
    case 322: return Op::VMAXSH;
    case 324: return Op::VSLH;
    case 328: return Op::VMULOSH;
    case 330: return Op::VRSQRTEFP;
    case 332: return Op::VMRGLH;
    case 334: return Op::VPKSWUS;
    case 384: return Op::VADDCUW;
    case 386: return Op::VMAXSW;
    case 388: return Op::VSLW;
    case 394: return Op::VEXPTEFP;
    case 396: return Op::VMRGLW;
    case 398: return Op::VPKSHSS;
    case 452: return Op::VSL;
    case 458: return Op::VLOGEFP;
    case 462: return Op::VPKSWSS;
    case 512: return Op::VADDUBS;
    case 514: return Op::VMINUB;
    case 516: return Op::VSRB;
    case 520: return Op::VMULEUB;
    case 522: return Op::VRFIN;
    case 524: return Op::VSPLTB;
    case 526: return Op::VUPKHSB;
    case 576: return Op::VADDUHS;
    case 578: return Op::VMINUH;
    case 580: return Op::VSRH;
    case 584: return Op::VMULEUH;
    case 586: return Op::VRFIZ;
    case 588: return Op::VSPLTH;
    case 590: return Op::VUPKHSH;
    case 640: return Op::VADDUWS;
    case 642: return Op::VMINUW;
    case 644: return Op::VSRW;
    case 650: return Op::VRFIP;
    case 652: return Op::VSPLTW;
    case 654: return Op::VUPKLSB;
    case 708: return Op::VSR;
    case 714: return Op::VRFIM;
    case 718: return Op::VUPKLSH;
    case 768: return Op::VADDSBS;
    case 770: return Op::VMINSB;
    case 772: return Op::VSRAB;
    case 776: return Op::VMULESB;
    case 778: return Op::VCFUX;
    case 780: return Op::VSPLTISB;
    case 782: return Op::VPKPX;
    case 832: return Op::VADDSHS;
    case 834: return Op::VMINSH;
    case 836: return Op::VSRAH;
    case 840: return Op::VMULESH;
    case 842: return Op::VCFSX;
    case 844: return Op::VSPLTISH;
    case 846: return Op::VUPKHPX;
    case 896: return Op::VADDSWS;
    case 898: return Op::VMINSW;
    case 900: return Op::VSRAW;
    case 906: return Op::VCTUXS;
    case 908: return Op::VSPLTISW;
    case 970: return Op::VCTSXS;
    case 974: return Op::VUPKLPX;
    case 1024: return Op::VSUBUBM;
    case 1026: return Op::VAVGUB;
    case 1028: return Op::VAND;
    case 1034: return Op::VMAXFP;
    case 1036: return Op::VSLO;
    case 1088: return Op::VSUBUHM;
    case 1090: return Op::VAVGUH;
    case 1092: return Op::VANDC;
    case 1098: return Op::VMINFP;
    case 1100: return Op::VSRO;
    case 1152: return Op::VSUBUWM;
    case 1154: return Op::VAVGUW;
    case 1156: return Op::VOR;
    case 1220: return Op::VXOR;
    case 1282: return Op::VAVGSB;
    case 1284: return Op::VNOR;
    case 1346: return Op::VAVGSH;
    case 1408: return Op::VSUBCUW;
    case 1410: return Op::VAVGSW;
    case 1536: return Op::VSUBUBS;
    case 1540: return Op::MFVSCR;
    case 1544: return Op::VSUM4UBS;
    case 1600: return Op::VSUBUHS;
    case 1604: return Op::MTVSCR;
    case 1608: return Op::VSUM4SHS;
    case 1664: return Op::VSUBUWS;
    case 1672: return Op::VSUM2SWS;
    case 1792: return Op::VSUBSBS;
    case 1800: return Op::VSUM4SBS;
    case 1856: return Op::VSUBSHS;
    case 1920: return Op::VSUBSWS;
    case 1928: return Op::VSUMSWS;
  }
  return Op::INVALID;
}

Op decode(u32 w) {
  switch (opcd(w)) {
    case 2: return Op::TDI;
    case 3: return Op::TWI;
    case 4: return decode4(w);
    case 7: return Op::MULLI;
    case 8: return Op::SUBFIC;
    case 10: return Op::CMPLI;
    case 11: return Op::CMPI;
    case 12: return Op::ADDIC;
    case 13: return Op::ADDIC_;
    case 14: return Op::ADDI;
    case 15: return Op::ADDIS;
    case 16: return Op::BC;
    case 17: return (w & 2) ? Op::SC : Op::INVALID;
    case 18: return Op::B;
    case 19: return decode19(w);
    case 20: return Op::RLWIMI;
    case 21: return Op::RLWINM;
    case 23: return Op::RLWNM;
    case 24: return Op::ORI;
    case 25: return Op::ORIS;
    case 26: return Op::XORI;
    case 27: return Op::XORIS;
    case 28: return Op::ANDI_;
    case 29: return Op::ANDIS_;
    case 30: return decode30(w);
    case 31: return decode31(w);
    case 32: return Op::LWZ;
    case 33: return Op::LWZU;
    case 34: return Op::LBZ;
    case 35: return Op::LBZU;
    case 36: return Op::STW;
    case 37: return Op::STWU;
    case 38: return Op::STB;
    case 39: return Op::STBU;
    case 40: return Op::LHZ;
    case 41: return Op::LHZU;
    case 42: return Op::LHA;
    case 43: return Op::LHAU;
    case 44: return Op::STH;
    case 45: return Op::STHU;
    case 46: return Op::LMW;
    case 47: return Op::STMW;
    case 48: return Op::LFS;
    case 49: return Op::LFSU;
    case 50: return Op::LFD;
    case 51: return Op::LFDU;
    case 52: return Op::STFS;
    case 53: return Op::STFSU;
    case 54: return Op::STFD;
    case 55: return Op::STFDU;
    case 58:
      switch (w & 3) {
        case 0: return Op::LD;
        case 1: return Op::LDU;
        case 2: return Op::LWA;
      }
      return Op::INVALID;
    case 59: return decode59(w);
    case 62:
      switch (w & 3) {
        case 0: return Op::STD;
        case 1: return Op::STDU;
      }
      return Op::INVALID;
    case 63: return decode63(w);
  }
  return Op::INVALID;
}

// ---------------------------------------------------------------------------------------------
// Disassembler (diagnostic quality; operand order follows the usual assembler syntax).

static void lower(const char* s, char* out) {
  while (*s) {
    char c = *s++;
    if (c == '_') c = '.';
    *out++ = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
  }
  *out = 0;
}

void disasm(u32 addr, u32 w, char* out, size_t n) {
  Op op = decode(w);
  char mn[32];
  lower(op_name(op), mn);
  u32 d = rd(w), a = ra(w), b = rb(w);
  const char* dot = rc(w) ? "." : "";
  switch (op) {
    case Op::INVALID: std::snprintf(out, n, ".long 0x%08x", w); return;
    case Op::B: {
      u32 t = (aa(w) ? 0 : addr) + li(w);
      std::snprintf(out, n, "b%s%s 0x%x", lk(w) ? "l" : "", aa(w) ? "a" : "", t);
      return;
    }
    case Op::BC: {
      u32 t = (aa(w) ? 0 : addr) + bd(w);
      std::snprintf(out, n, "bc%s%s %u,%u,0x%x", lk(w) ? "l" : "", aa(w) ? "a" : "", d, a, t);
      return;
    }
    case Op::BCLR:
      if (d == 20) std::snprintf(out, n, "blr%s", lk(w) ? "l" : "");
      else std::snprintf(out, n, "bclr%s %u,%u", lk(w) ? "l" : "", d, a);
      return;
    case Op::BCCTR:
      if (d == 20) std::snprintf(out, n, "bctr%s", lk(w) ? "l" : "");
      else std::snprintf(out, n, "bcctr%s %u,%u", lk(w) ? "l" : "", d, a);
      return;
    case Op::SC: std::snprintf(out, n, "sc"); return;
    case Op::ADDI:
      if (a == 0) std::snprintf(out, n, "li r%u,%d", d, simm(w));
      else std::snprintf(out, n, "addi r%u,r%u,%d", d, a, simm(w));
      return;
    case Op::ADDIS:
      if (a == 0) std::snprintf(out, n, "lis r%u,0x%x", d, uimm(w));
      else std::snprintf(out, n, "addis r%u,r%u,0x%x", d, a, uimm(w));
      return;
    case Op::ORI:
      if (w == 0x60000000) std::snprintf(out, n, "nop");
      else std::snprintf(out, n, "ori r%u,r%u,0x%x", a, d, uimm(w));
      return;
    case Op::ORIS: case Op::XORI: case Op::XORIS: case Op::ANDI_: case Op::ANDIS_:
      std::snprintf(out, n, "%s r%u,r%u,0x%x", mn, a, d, uimm(w));
      return;
    case Op::MULLI: case Op::SUBFIC: case Op::ADDIC: case Op::ADDIC_:
      std::snprintf(out, n, "%s r%u,r%u,%d", mn, d, a, simm(w));
      return;
    case Op::CMPI: std::snprintf(out, n, "cmp%si cr%u,r%u,%d", cmp_l(w) ? "d" : "w", crfd(w), a, simm(w)); return;
    case Op::CMPLI: std::snprintf(out, n, "cmpl%si cr%u,r%u,0x%x", cmp_l(w) ? "d" : "w", crfd(w), a, uimm(w)); return;
    case Op::CMP: std::snprintf(out, n, "cmp%s cr%u,r%u,r%u", cmp_l(w) ? "d" : "w", crfd(w), a, b); return;
    case Op::CMPL: std::snprintf(out, n, "cmpl%s cr%u,r%u,r%u", cmp_l(w) ? "d" : "w", crfd(w), a, b); return;
    case Op::RLWINM: case Op::RLWIMI:
      std::snprintf(out, n, "%s%s r%u,r%u,%u,%u,%u", mn, dot, a, d, b, rc_(w), me(w));
      return;
    case Op::RLWNM: std::snprintf(out, n, "rlwnm%s r%u,r%u,r%u,%u,%u", dot, a, d, b, rc_(w), me(w)); return;
    case Op::RLDICL: case Op::RLDICR: case Op::RLDIC: case Op::RLDIMI:
      std::snprintf(out, n, "%s%s r%u,r%u,%u,%u", mn, dot, a, d, md_sh(w), md_mb(w));
      return;
    case Op::RLDCL: case Op::RLDCR: std::snprintf(out, n, "%s%s r%u,r%u,r%u,%u", mn, dot, a, d, b, md_mb(w)); return;
    case Op::SRAWI: std::snprintf(out, n, "srawi%s r%u,r%u,%u", dot, a, d, b); return;
    case Op::SRADI: std::snprintf(out, n, "sradi%s r%u,r%u,%u", dot, a, d, md_sh(w)); return;
    case Op::OR:
      if (d == b) std::snprintf(out, n, "mr%s r%u,r%u", dot, a, d);
      else std::snprintf(out, n, "or%s r%u,r%u,r%u", dot, a, d, b);
      return;
    case Op::AND: case Op::ANDC: case Op::NOR: case Op::XOR: case Op::EQV: case Op::ORC: case Op::NAND:
    case Op::SLW: case Op::SRW: case Op::SLD: case Op::SRD: case Op::SRAW: case Op::SRAD:
      std::snprintf(out, n, "%s%s r%u,r%u,r%u", mn, dot, a, d, b);
      return;
    case Op::CNTLZW: case Op::CNTLZD: case Op::EXTSB: case Op::EXTSH: case Op::EXTSW:
      std::snprintf(out, n, "%s%s r%u,r%u", mn, dot, a, d);
      return;
    case Op::NEG: case Op::ADDZE: case Op::ADDME: case Op::SUBFZE: case Op::SUBFME:
      std::snprintf(out, n, "%s%s%s r%u,r%u", mn, oe(w) ? "o" : "", dot, d, a);
      return;
    case Op::ADD: case Op::ADDC: case Op::ADDE: case Op::SUBF: case Op::SUBFC: case Op::SUBFE: case Op::MULLW:
    case Op::MULLD: case Op::DIVW: case Op::DIVWU: case Op::DIVD: case Op::DIVDU: case Op::MULHW: case Op::MULHWU:
    case Op::MULHD: case Op::MULHDU:
      std::snprintf(out, n, "%s%s%s r%u,r%u,r%u", mn, oe(w) ? "o" : "", dot, d, a, b);
      return;
    case Op::MFSPR: std::snprintf(out, n, "mfspr r%u,%u", d, spr(w)); return;
    case Op::MTSPR: std::snprintf(out, n, "mtspr %u,r%u", spr(w), d); return;
    case Op::MFTB: std::snprintf(out, n, "mftb r%u,%u", d, spr(w)); return;
    case Op::MFCR: case Op::MFOCRF: std::snprintf(out, n, "%s r%u", mn, d); return;
    case Op::MTCRF: case Op::MTOCRF: std::snprintf(out, n, "%s 0x%x,r%u", mn, crm(w), d); return;
    case Op::LWZ: case Op::LWZU: case Op::LBZ: case Op::LBZU: case Op::STW: case Op::STWU: case Op::STB:
    case Op::STBU: case Op::LHZ: case Op::LHZU: case Op::LHA: case Op::LHAU: case Op::STH: case Op::STHU:
    case Op::LMW: case Op::STMW:
      std::snprintf(out, n, "%s r%u,%d(r%u)", mn, d, simm(w), a);
      return;
    case Op::LFS: case Op::LFSU: case Op::LFD: case Op::LFDU: case Op::STFS: case Op::STFSU: case Op::STFD:
    case Op::STFDU:
      std::snprintf(out, n, "%s f%u,%d(r%u)", mn, d, simm(w), a);
      return;
    case Op::LD: case Op::LDU: case Op::LWA: case Op::STD: case Op::STDU:
      std::snprintf(out, n, "%s r%u,%d(r%u)", mn, d, ds(w), a);
      return;
    case Op::LFSX: case Op::LFSUX: case Op::LFDX: case Op::LFDUX: case Op::STFSX: case Op::STFSUX: case Op::STFDX:
    case Op::STFDUX: case Op::STFIWX:
      std::snprintf(out, n, "%s f%u,r%u,r%u", mn, d, a, b);
      return;
    case Op::LVX: case Op::LVXL: case Op::STVX: case Op::STVXL: case Op::LVEBX: case Op::LVEHX: case Op::LVEWX:
    case Op::STVEBX: case Op::STVEHX: case Op::STVEWX: case Op::LVSL: case Op::LVSR: case Op::LVLX: case Op::LVRX:
    case Op::LVLXL: case Op::LVRXL: case Op::STVLX: case Op::STVRX: case Op::STVLXL: case Op::STVRXL:
      std::snprintf(out, n, "%s v%u,r%u,r%u", mn, d, a, b);
      return;
    case Op::SYNC: case Op::EIEIO: case Op::ISYNC: std::snprintf(out, n, "%s", mn); return;
    case Op::MCRF: std::snprintf(out, n, "mcrf cr%u,cr%u", crfd(w), crfs(w)); return;
    case Op::CRNOR: case Op::CRANDC: case Op::CRXOR: case Op::CRNAND: case Op::CRAND: case Op::CREQV:
    case Op::CRORC: case Op::CROR:
      std::snprintf(out, n, "%s %u,%u,%u", mn, d, a, b);
      return;
    default: break;
  }
  u32 o = opcd(w);
  if (o == 59 || o == 63) {
    std::snprintf(out, n, "%s%s f%u,f%u,f%u,f%u", mn, dot, d, a, b, rc_(w));
    return;
  }
  if (o == 4) {
    std::snprintf(out, n, "%s v%u,v%u,v%u,v%u/%u", mn, d, a, b, rc_(w), vuimm(w));
    return;
  }
  std::snprintf(out, n, "%s r%u,r%u,r%u", mn, d, a, b);
}

}  // namespace ppu
