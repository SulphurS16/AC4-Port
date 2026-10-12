// C++ emission for analysed PPU functions. Generated code is game-derived: it is written under
// local/gen and never committed or distributed.
#include "recomp/emit.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "common/file.h"

namespace recomp {

using namespace ppu;
using ac4::strprintf;

namespace {

struct FnEmitter {
  const Image& img;
  const Analysis& an;
  const Function& f;
  std::set<u32>& refs;
  bool comments;
  std::string out;

  bool is_func(u32 a) const { return an.functions().count(a) != 0; }
  std::string call(u32 t) {
    if (is_func(t)) {
      refs.insert(t);
      return strprintf("f_%08x(ctx);", t);
    }
    return strprintf("icall(ctx,0x%08xu);", t);
  }
  // Jump to t: goto inside the function, otherwise tail call.
  std::string jump(u32 t) {
    if (f.labels.count(t) && t >= f.begin && t < f.end) return strprintf("goto L_%08x;", t);
    return "{" + call(t) + "return;}";
  }
  static std::string cond(u32 bo, u32 bi) {
    std::string c;
    if (!(bo & 4)) c = (bo & 2) ? "--ctx.ctr==0" : "--ctx.ctr!=0";
    if (!(bo & 16)) {
      std::string t = strprintf((bo & 8) ? "ctx.cr[%u]" : "!ctx.cr[%u]", bi);
      c = c.empty() ? t : c + "&&" + t;
    }
    return c;
  }
  static std::string ra0(u32 a) { return a ? strprintf("R(%u)", a) : std::string("0"); }
  static std::string ead(u32 a, s32 d) {
    if (!a) return strprintf("0x%xu", u32(d));
    return d ? strprintf("(u32(R(%u))+%du)", a, d) : strprintf("u32(R(%u))", a);
  }
  static std::string eax(u32 a, u32 b) { return a ? strprintf("u32(R(%u)+R(%u))", a, b) : strprintf("u32(R(%u))", b); }

  void line(const std::string& s) {
    out += "  ";
    out += s;
    out += '\n';
  }

  // Returns true if control can fall through to pc+4.
  bool insn(u32 pc, u32 w);
  void body(const std::string& hook);
};

bool FnEmitter::insn(u32 pc, u32 w) {
  Op op = decode(w);
  u32 d = rd(w), a = ra(w), b = rb(w), c = rc_(w);
  s32 si = simm(w);
  u32 ui = uimm(w);
  bool rec = rc(w);
  auto CR0 = [&](u32 reg) { return rec ? strprintf("cr0(ctx,R(%u));", reg) : std::string(); };
  auto OV = [&](const std::string& expr) { return oe(w) ? "set_ov(ctx," + expr + ");" : std::string(); };
  // Logical/rotate results land in rA (from rS = d).
  auto L = [&](const std::string& expr) { line(strprintf("R(%u)=%s;", a, expr.c_str()) + CR0(a)); };
  auto D = [&](const std::string& expr) { line(strprintf("R(%u)=%s;", d, expr.c_str()) + CR0(d)); };
  auto FD = [&](const std::string& expr) { line(strprintf("F(%u)=%s;", d, expr.c_str()) + (rec ? "cr1_fp(ctx);" : "")); };
  auto VD = [&](const std::string& expr) { line(strprintf("V(%u)=%s;", d, expr.c_str())); };
  auto V2 = [&](const char* fn) { VD(strprintf("%s(V(%u),V(%u))", fn, a, b)); };
  auto VI = [&](const char* intr) { VD(strprintf("mk(%s(V(%u).vi,V(%u).vi))", intr, a, b)); };
  auto VF = [&](const char* intr) { VD(strprintf("mkf(%s(V(%u).vf,V(%u).vf))", intr, a, b)); };
  auto VCMP = [&](const std::string& expr) {
    line(strprintf("V(%u)=%s;", d, expr.c_str()) + (vrc(w) ? strprintf("cr6(ctx,V(%u));", d) : ""));
  };
  // Loads / stores.
  auto LOAD = [&](const char* fn, const std::string& ea, const char* conv = "") {
    line(strprintf("R(%u)=%s%s(%s);", d, conv, fn, ea.c_str()));
  };
  auto LOADU = [&](const char* fn, const std::string& ea, const char* conv = "") {
    line(strprintf("{u32 ea=%s;R(%u)=%s%s(ea);R(%u)=ea;}", ea.c_str(), d, conv, fn, a));
  };
  auto STORE = [&](const char* fn, const std::string& ea, const char* cast) {
    line(strprintf("%s(%s,%s(R(%u)));", fn, ea.c_str(), cast, d));
  };
  auto STOREU = [&](const char* fn, const std::string& ea, const char* cast) {
    line(strprintf("{u32 ea=%s;%s(ea,%s(R(%u)));R(%u)=ea;}", ea.c_str(), fn, cast, d, a));
  };

  switch (op) {
    case Op::INVALID:
      line(strprintf("trap(ctx,0x%08xu,\"invalid instruction\");", pc));
      return false;
    // ---- traps ----
    case Op::TWI:
      if (d == 31) { line(strprintf("trap(ctx,0x%08xu,\"twi\");", pc)); return false; }
      line(strprintf("if(trap_if(%u,s64(s32(R(%u))),%d))trap(ctx,0x%08xu,\"twi\");", d, a, si, pc));
      return true;
    case Op::TDI:
      if (d == 31) { line(strprintf("trap(ctx,0x%08xu,\"tdi\");", pc)); return false; }
      line(strprintf("if(trap_if(%u,s64(R(%u)),%d))trap(ctx,0x%08xu,\"tdi\");", d, a, si, pc));
      return true;
    case Op::TW:
      if (d == 31) { line(strprintf("trap(ctx,0x%08xu,\"tw\");", pc)); return false; }
      line(strprintf("if(trap_if(%u,s64(s32(R(%u))),s64(s32(R(%u)))))trap(ctx,0x%08xu,\"tw\");", d, a, b, pc));
      return true;
    case Op::TD:
      if (d == 31) { line(strprintf("trap(ctx,0x%08xu,\"td\");", pc)); return false; }
      line(strprintf("if(trap_if(%u,s64(R(%u)),s64(R(%u))))trap(ctx,0x%08xu,\"td\");", d, a, b, pc));
      return true;
    // ---- integer immediate ----
    case Op::MULLI: D(strprintf("u64(s64(R(%u))*%d)", a, si)); return true;
    case Op::SUBFIC:
      line(strprintf("{u64 x=R(%u),i=u64(s64(%d));ctx.ca=i>=x;R(%u)=i-x;}", a, si, d));
      return true;
    case Op::CMPLI:
      line(cmp_l(w) ? strprintf("crset<u64>(ctx,%u,R(%u),0x%xull);", crfd(w), a, ui)
                    : strprintf("crset<u32>(ctx,%u,u32(R(%u)),0x%xu);", crfd(w), a, ui));
      return true;
    case Op::CMPI:
      line(cmp_l(w) ? strprintf("crset<s64>(ctx,%u,s64(R(%u)),s64(%d));", crfd(w), a, si)
                    : strprintf("crset<s32>(ctx,%u,s32(R(%u)),%d);", crfd(w), a, si));
      return true;
    case Op::ADDIC:
    case Op::ADDIC_:
      line(strprintf("{u64 x=R(%u),r=x+u64(s64(%d));ctx.ca=r<x;R(%u)=r;}", a, si, d) + (op == Op::ADDIC_ ? strprintf("cr0(ctx,R(%u));", d) : ""));
      return true;
    case Op::ADDI:
      line(a ? strprintf("R(%u)=R(%u)+u64(s64(%d));", d, a, si) : strprintf("R(%u)=u64(s64(%d));", d, si));
      return true;
    case Op::ADDIS:
      line(a ? strprintf("R(%u)=R(%u)+u64(s64(%d)<<16);", d, a, si) : strprintf("R(%u)=u64(s64(%d)<<16);", d, si));
      return true;
    // ---- branches ----
    case Op::B: {
      u32 t = (aa(w) ? 0 : pc) + u32(li(w));
      if (lk(w)) {
        line(strprintf("ctx.lr=0x%08xu;", pc + 4) + call(t));
        return true;
      }
      line(jump(t));
      return false;
    }
    case Op::BC: {
      u32 t = (aa(w) ? 0 : pc) + u32(bd(w));
      std::string cnd = cond(d, a);
      if (lk(w)) {
        std::string body = strprintf("ctx.lr=0x%08xu;", pc + 4) + (t == pc + 4 ? std::string() : call(t));
        line(cnd.empty() ? body : "if(" + cnd + "){" + body + "}");
        return true;
      }
      if (cnd.empty()) {
        line(jump(t));
        return false;
      }
      line("if(" + cnd + ")" + jump(t));
      return true;
    }
    case Op::BCLR: {
      std::string cnd = cond(d, a);
      if (lk(w)) {
        std::string body = strprintf("{u32 t=u32(ctx.lr);ctx.lr=0x%08xu;ctx.cia=0x%08xu;icall(ctx,t);}", pc + 4, pc);
        line(cnd.empty() ? body : "if(" + cnd + ")" + body);
        return true;
      }
      if (cnd.empty()) {
        line("return;");
        return false;
      }
      line("if(" + cnd + ")return;");
      return true;
    }
    case Op::BCCTR: {
      std::string cnd = cond(d | 4, a);  // bcctr never decrements CTR
      if (lk(w)) {
        std::string body = strprintf("{ctx.lr=0x%08xu;ctx.cia=0x%08xu;icall(ctx,u32(ctx.ctr));}", pc + 4, pc);
        line(cnd.empty() ? body : "if(" + cnd + ")" + body);
        return true;
      }
      auto sw = f.switches.find(pc);
      std::string body;
      if (sw != f.switches.end()) {
        body = "switch(u32(ctx.ctr)){";
        std::set<u32> seen;
        for (u32 t : sw->second)
          if (seen.insert(t).second) body += strprintf("case 0x%08xu:goto L_%08x;", t, t);
        body += strprintf("default:bad_switch(ctx,0x%08xu,u32(ctx.ctr));return;}", pc);
      } else {
        body = strprintf("{ctx.cia=0x%08xu;icall(ctx,u32(ctx.ctr));return;}", pc);
      }
      if (cnd.empty()) {
        line(body);
        return false;
      }
      line("if(" + cnd + ")" + body);
      return true;
    }
    case Op::SC: line(strprintf("ctx.cia=0x%08xu;syscall(ctx);", pc)); return true;
    // ---- condition register ----
    case Op::MCRF: line(strprintf("std::memcpy(&ctx.cr[%u],&ctx.cr[%u],4);", 4 * crfd(w), 4 * crfs(w))); return true;
    case Op::CRNOR: line(strprintf("ctx.cr[%u]=!(ctx.cr[%u]|ctx.cr[%u]);", d, a, b)); return true;
    case Op::CRANDC: line(strprintf("ctx.cr[%u]=ctx.cr[%u]&!ctx.cr[%u];", d, a, b)); return true;
    case Op::CRXOR: line(strprintf("ctx.cr[%u]=ctx.cr[%u]^ctx.cr[%u];", d, a, b)); return true;
    case Op::CRNAND: line(strprintf("ctx.cr[%u]=!(ctx.cr[%u]&ctx.cr[%u]);", d, a, b)); return true;
    case Op::CRAND: line(strprintf("ctx.cr[%u]=ctx.cr[%u]&ctx.cr[%u];", d, a, b)); return true;
    case Op::CREQV: line(strprintf("ctx.cr[%u]=!(ctx.cr[%u]^ctx.cr[%u]);", d, a, b)); return true;
    case Op::CRORC: line(strprintf("ctx.cr[%u]=ctx.cr[%u]|!ctx.cr[%u];", d, a, b)); return true;
    case Op::CROR: line(strprintf("ctx.cr[%u]=ctx.cr[%u]|ctx.cr[%u];", d, a, b)); return true;
    case Op::ISYNC: return true;
    // ---- rotates / logical immediates ----
    case Op::RLWIMI: {
      u64 m = mask64(c + 32, me(w) + 32);
      L(strprintf("(rotl32d(R(%u),%u)&0x%llxull)|(R(%u)&0x%llxull)", d, b, (unsigned long long)m, a, (unsigned long long)~m));
      return true;
    }
    case Op::RLWINM: {
      u64 m = mask64(c + 32, me(w) + 32);
      if (b == 0 && c <= me(w)) L(strprintf("R(%u)&0x%llxull", d, (unsigned long long)m));
      else L(strprintf("rotl32d(R(%u),%u)&0x%llxull", d, b, (unsigned long long)m));
      return true;
    }
    case Op::RLWNM: {
      u64 m = mask64(c + 32, me(w) + 32);
      L(strprintf("rotl32d(R(%u),u32(R(%u)))&0x%llxull", d, b, (unsigned long long)m));
      return true;
    }
    case Op::ORI:
      if (w == 0x60000000) return true;  // nop
      L(strprintf("R(%u)|0x%xull", d, ui));
      return true;
    case Op::ORIS: L(strprintf("R(%u)|0x%xull", d, ui << 16)); return true;
    case Op::XORI: L(strprintf("R(%u)^0x%xull", d, ui)); return true;
    case Op::XORIS: L(strprintf("R(%u)^0x%xull", d, ui << 16)); return true;
    case Op::ANDI_: line(strprintf("R(%u)=R(%u)&0x%xull;cr0(ctx,R(%u));", a, d, ui, a)); return true;
    case Op::ANDIS_: line(strprintf("R(%u)=R(%u)&0x%xull;cr0(ctx,R(%u));", a, d, ui << 16, a)); return true;
    case Op::RLDICL: {
      u32 sh = md_sh(w), mb = md_mb(w);
      u64 m = mask64(mb, 63);
      L(sh ? strprintf("rotl64(R(%u),%u)&0x%llxull", d, sh, (unsigned long long)m) : strprintf("R(%u)&0x%llxull", d, (unsigned long long)m));
      return true;
    }
    case Op::RLDICR: {
      u32 sh = md_sh(w), e = md_mb(w);
      L(strprintf("rotl64(R(%u),%u)&0x%llxull", d, sh, (unsigned long long)mask64(0, e)));
      return true;
    }
    case Op::RLDIC: {
      u32 sh = md_sh(w), mb = md_mb(w);
      L(strprintf("rotl64(R(%u),%u)&0x%llxull", d, sh, (unsigned long long)mask64(mb, 63 - sh)));
      return true;
    }
    case Op::RLDIMI: {
      u32 sh = md_sh(w), mb = md_mb(w);
      u64 m = mask64(mb, 63 - sh);
      L(strprintf("(rotl64(R(%u),%u)&0x%llxull)|(R(%u)&0x%llxull)", d, sh, (unsigned long long)m, a, (unsigned long long)~m));
      return true;
    }
    case Op::RLDCL: L(strprintf("rotl64(R(%u),u32(R(%u)))&0x%llxull", d, b, (unsigned long long)mask64(md_mb(w), 63))); return true;
    case Op::RLDCR: L(strprintf("rotl64(R(%u),u32(R(%u)))&0x%llxull", d, b, (unsigned long long)mask64(0, md_mb(w)))); return true;
    // ---- opcode 31 integer ----
    case Op::CMP:
      line(cmp_l(w) ? strprintf("crset<s64>(ctx,%u,s64(R(%u)),s64(R(%u)));", crfd(w), a, b)
                    : strprintf("crset<s32>(ctx,%u,s32(R(%u)),s32(R(%u)));", crfd(w), a, b));
      return true;
    case Op::CMPL:
      line(cmp_l(w) ? strprintf("crset<u64>(ctx,%u,R(%u),R(%u));", crfd(w), a, b)
                    : strprintf("crset<u32>(ctx,%u,u32(R(%u)),u32(R(%u)));", crfd(w), a, b));
      return true;
    case Op::SUBFC:
      line(strprintf("{u64 x=R(%u),y=R(%u),r=y-x;ctx.ca=y>=x;", a, b) + OV("u8((((x^y)&(y^r))>>63)&1)") +
           strprintf("R(%u)=r;}", d) + CR0(d));
      return true;
    case Op::ADDC:
      line(strprintf("{u64 x=R(%u),y=R(%u),r=x+y;ctx.ca=r<x;", a, b) + OV("ovf_add(x,y,r)") + strprintf("R(%u)=r;}", d) + CR0(d));
      return true;
    case Op::SUBF:
      line(strprintf("{u64 x=R(%u),y=R(%u),r=y-x;", a, b) + OV("u8((((x^y)&(y^r))>>63)&1)") + strprintf("R(%u)=r;}", d) + CR0(d));
      return true;
    case Op::ADD:
      line(strprintf("{u64 x=R(%u),y=R(%u),r=x+y;", a, b) + OV("ovf_add(x,y,r)") + strprintf("R(%u)=r;}", d) + CR0(d));
      return true;
    case Op::NEG:
      line(strprintf("{u64 x=R(%u);", a) + OV("u8(x==0x8000000000000000ull)") + strprintf("R(%u)=0-x;}", d) + CR0(d));
      return true;
    case Op::ADDE: case Op::SUBFE: case Op::ADDZE: case Op::SUBFZE: case Op::ADDME: case Op::SUBFME: {
      const char* x = (op == Op::SUBFE || op == Op::SUBFZE || op == Op::SUBFME) ? "~" : "";
      std::string y = (op == Op::ADDE || op == Op::SUBFE) ? strprintf("R(%u)", b) : (op == Op::ADDZE || op == Op::SUBFZE) ? "0ull" : "~0ull";
      line(strprintf("{u64 x=%sR(%u),y=%s;u8 co;u64 r=add_c(x,y,ctx.ca,co);ctx.ca=co;", x, a, y.c_str()) + OV("ovf_add(x,y,r)") +
           strprintf("R(%u)=r;}", d) + CR0(d));
      return true;
    }
    case Op::MULLD:
      line(strprintf("{u64 x=R(%u),y=R(%u);", a, b) +
           OV("u8(__int128(s64(x))*s64(y)!=__int128(s64(x*y)))") + strprintf("R(%u)=x*y;}", d) + CR0(d));
      return true;
    case Op::MULLW:
      line(strprintf("{s64 p=s64(s32(R(%u)))*s32(R(%u));", a, b) + OV("u8(p!=s64(s32(p)))") + strprintf("R(%u)=u64(p);}", d) + CR0(d));
      return true;
    case Op::MULHW: D(strprintf("mulhw(R(%u),R(%u))", a, b)); return true;
    case Op::MULHWU: D(strprintf("mulhwu(R(%u),R(%u))", a, b)); return true;
    case Op::MULHD: D(strprintf("mulhd(R(%u),R(%u))", a, b)); return true;
    case Op::MULHDU: D(strprintf("mulhdu(R(%u),R(%u))", a, b)); return true;
    case Op::DIVW:
      line(strprintf("{u64 x=R(%u),y=R(%u);", a, b) + OV("u8(s32(y)==0||(s32(x)==INT32_MIN&&s32(y)==-1))") +
           strprintf("R(%u)=divw(x,y);}", d) + CR0(d));
      return true;
    case Op::DIVWU:
      line(strprintf("{u64 x=R(%u),y=R(%u);", a, b) + OV("u8(u32(y)==0)") + strprintf("R(%u)=divwu(x,y);}", d) + CR0(d));
      return true;
    case Op::DIVD:
      line(strprintf("{u64 x=R(%u),y=R(%u);", a, b) + OV("u8(y==0||(s64(x)==INT64_MIN&&s64(y)==-1))") +
           strprintf("R(%u)=divd(x,y);}", d) + CR0(d));
      return true;
    case Op::DIVDU:
      line(strprintf("{u64 x=R(%u),y=R(%u);", a, b) + OV("u8(y==0)") + strprintf("R(%u)=divdu(x,y);}", d) + CR0(d));
      return true;
    case Op::AND: L(strprintf("R(%u)&R(%u)", d, b)); return true;
    case Op::ANDC: L(strprintf("R(%u)&~R(%u)", d, b)); return true;
    case Op::OR: L(d == b ? strprintf("R(%u)", d) : strprintf("R(%u)|R(%u)", d, b)); return true;
    case Op::ORC: L(strprintf("R(%u)|~R(%u)", d, b)); return true;
    case Op::XOR: L(strprintf("R(%u)^R(%u)", d, b)); return true;
    case Op::NOR: L(strprintf("~(R(%u)|R(%u))", d, b)); return true;
    case Op::NAND: L(strprintf("~(R(%u)&R(%u))", d, b)); return true;
    case Op::EQV: L(strprintf("~(R(%u)^R(%u))", d, b)); return true;
    case Op::SLW: L(strprintf("slw(R(%u),R(%u))", d, b)); return true;
    case Op::SRW: L(strprintf("srw(R(%u),R(%u))", d, b)); return true;
    case Op::SLD: L(strprintf("sld(R(%u),R(%u))", d, b)); return true;
    case Op::SRD: L(strprintf("srd(R(%u),R(%u))", d, b)); return true;
    case Op::SRAW: L(strprintf("sraw(ctx,R(%u),u32(R(%u))&63)", d, b)); return true;
    case Op::SRAD: L(strprintf("srad(ctx,R(%u),u32(R(%u))&127)", d, b)); return true;
    case Op::SRAWI: L(strprintf("sraw(ctx,R(%u),%u)", d, b)); return true;
    case Op::SRADI: L(strprintf("srad(ctx,R(%u),%u)", d, md_sh(w))); return true;
    case Op::CNTLZW: L(strprintf("cntlzw(R(%u))", d)); return true;
    case Op::CNTLZD: L(strprintf("cntlzd(R(%u))", d)); return true;
    case Op::EXTSB: L(strprintf("u64(s64(s8(R(%u))))", d)); return true;
    case Op::EXTSH: L(strprintf("u64(s64(s16(R(%u))))", d)); return true;
    case Op::EXTSW: L(strprintf("u64(s64(s32(R(%u))))", d)); return true;
    case Op::MFCR: case Op::MFOCRF: line(strprintf("R(%u)=mfcr(ctx);", d)); return true;
    case Op::MTCRF: case Op::MTOCRF: line(strprintf("mtcrf(ctx,0x%x,R(%u));", crm(w), d)); return true;
    case Op::MFSPR: {
      u32 s = spr(w);
      if (s == 1) line(strprintf("R(%u)=mfxer(ctx);", d));
      else if (s == 8) line(strprintf("R(%u)=ctx.lr;", d));
      else if (s == 9) line(strprintf("R(%u)=ctx.ctr;", d));
      else if (s == 256) line(strprintf("R(%u)=ctx.vrsave;", d));
      else if (s == 268) line(strprintf("R(%u)=timebase();", d));
      else if (s == 269) line(strprintf("R(%u)=timebase()>>32;", d));
      else line(strprintf("R(%u)=read_spr(ctx,0x%08xu,%u);", d, pc, s));
      return true;
    }
    case Op::MTSPR: {
      u32 s = spr(w);
      if (s == 1) line(strprintf("mtxer(ctx,R(%u));", d));
      else if (s == 8) line(strprintf("ctx.lr=R(%u);", d));
      else if (s == 9) line(strprintf("ctx.ctr=R(%u);", d));
      else if (s == 256) line(strprintf("ctx.vrsave=u32(R(%u));", d));
      else line(strprintf("write_spr(ctx,0x%08xu,%u,R(%u));", pc, s, d));
      return true;
    }
    case Op::MFTB:
      line(spr(w) == 269 ? strprintf("R(%u)=timebase()>>32;", d) : strprintf("R(%u)=timebase();", d));
      return true;
    case Op::MFMSR: case Op::MTMSR: case Op::MTMSRD: case Op::ECIWX: case Op::ECOWX:
      line(strprintf("trap(ctx,0x%08xu,\"privileged/unsupported %s\");", pc, op_name(op)));
      return false;
    // ---- loads / stores ----
    case Op::LBZ: LOAD("r8", ead(a, si)); return true;
    case Op::LHZ: LOAD("r16", ead(a, si)); return true;
    case Op::LHA: LOAD("r16", ead(a, si), "(u64)(s64)(s16)"); return true;
    case Op::LWZ: LOAD("r32", ead(a, si)); return true;
    case Op::LWA: LOAD("r32", ead(a, ds(w)), "(u64)(s64)(s32)"); return true;
    case Op::LD: LOAD("r64", ead(a, ds(w))); return true;
    case Op::LBZU: LOADU("r8", ead(a, si)); return true;
    case Op::LHZU: LOADU("r16", ead(a, si)); return true;
    case Op::LHAU: LOADU("r16", ead(a, si), "(u64)(s64)(s16)"); return true;
    case Op::LWZU: LOADU("r32", ead(a, si)); return true;
    case Op::LDU: LOADU("r64", ead(a, ds(w))); return true;
    case Op::LBZX: LOAD("r8", eax(a, b)); return true;
    case Op::LHZX: LOAD("r16", eax(a, b)); return true;
    case Op::LHAX: LOAD("r16", eax(a, b), "(u64)(s64)(s16)"); return true;
    case Op::LWZX: LOAD("r32", eax(a, b)); return true;
    case Op::LWAX: LOAD("r32", eax(a, b), "(u64)(s64)(s32)"); return true;
    case Op::LDX: LOAD("r64", eax(a, b)); return true;
    case Op::LBZUX: LOADU("r8", eax(a, b)); return true;
    case Op::LHZUX: LOADU("r16", eax(a, b)); return true;
    case Op::LHAUX: LOADU("r16", eax(a, b), "(u64)(s64)(s16)"); return true;
    case Op::LWZUX: LOADU("r32", eax(a, b)); return true;
    case Op::LWAUX: LOADU("r32", eax(a, b), "(u64)(s64)(s32)"); return true;
    case Op::LDUX: LOADU("r64", eax(a, b)); return true;
    case Op::LHBRX: LOAD("r16le", eax(a, b)); return true;
    case Op::LWBRX: LOAD("r32le", eax(a, b)); return true;
    case Op::LDBRX: LOAD("r64le", eax(a, b)); return true;
    case Op::STB: STORE("w8", ead(a, si), "u8"); return true;
    case Op::STH: STORE("w16", ead(a, si), "u16"); return true;
    case Op::STW: STORE("w32", ead(a, si), "u32"); return true;
    case Op::STD: STORE("w64", ead(a, ds(w)), "u64"); return true;
    case Op::STBU: STOREU("w8", ead(a, si), "u8"); return true;
    case Op::STHU: STOREU("w16", ead(a, si), "u16"); return true;
    case Op::STWU: STOREU("w32", ead(a, si), "u32"); return true;
    case Op::STDU: STOREU("w64", ead(a, ds(w)), "u64"); return true;
    case Op::STBX: STORE("w8", eax(a, b), "u8"); return true;
    case Op::STHX: STORE("w16", eax(a, b), "u16"); return true;
    case Op::STWX: STORE("w32", eax(a, b), "u32"); return true;
    case Op::STDX: STORE("w64", eax(a, b), "u64"); return true;
    case Op::STBUX: STOREU("w8", eax(a, b), "u8"); return true;
    case Op::STHUX: STOREU("w16", eax(a, b), "u16"); return true;
    case Op::STWUX: STOREU("w32", eax(a, b), "u32"); return true;
    case Op::STDUX: STOREU("w64", eax(a, b), "u64"); return true;
    case Op::STHBRX: STORE("w16le", eax(a, b), "u16"); return true;
    case Op::STWBRX: STORE("w32le", eax(a, b), "u32"); return true;
    case Op::STDBRX: STORE("w64le", eax(a, b), "u64"); return true;
    case Op::LMW: {
      std::string s = strprintf("{u32 ea=%s;", ead(a, si).c_str());
      for (u32 r = d; r < 32; ++r) s += strprintf("R(%u)=r32(ea+%u);", r, 4 * (r - d));
      line(s + "}");
      return true;
    }
    case Op::STMW: {
      std::string s = strprintf("{u32 ea=%s;", ead(a, si).c_str());
      for (u32 r = d; r < 32; ++r) s += strprintf("w32(ea+%u,u32(R(%u)));", 4 * (r - d), r);
      line(s + "}");
      return true;
    }
    case Op::LSWI: line(strprintf("lswi(ctx,%s,%u,%u);", (a ? strprintf("u32(R(%u))", a) : std::string("0")).c_str(), d, b)); return true;
    case Op::STSWI: line(strprintf("stswi(ctx,%s,%u,%u);", (a ? strprintf("u32(R(%u))", a) : std::string("0")).c_str(), d, b)); return true;
    case Op::LSWX: line(strprintf("lswi(ctx,%s,%u,ctx.xer_cnt?ctx.xer_cnt:0);", eax(a, b).c_str(), d)); return true;
    case Op::STSWX: line(strprintf("if(ctx.xer_cnt)stswi(ctx,%s,%u,ctx.xer_cnt);", eax(a, b).c_str(), d)); return true;
    case Op::LWARX:
      line(strprintf("R(%u)=lwarx32(ctx,%s);", d, eax(a, b).c_str()));
      return true;
    case Op::LDARX:
      line(strprintf("R(%u)=ldarx64(ctx,%s);", d, eax(a, b).c_str()));
      return true;
    case Op::STWCX:
      line(strprintf("{bool ok=stcx32(ctx,%s,u32(R(%u)));ctx.cr[0]=0;ctx.cr[1]=0;ctx.cr[2]=ok;ctx.cr[3]=ctx.so;}", eax(a, b).c_str(), d));
      return true;
    case Op::STDCX:
      line(strprintf("{bool ok=stcx64(ctx,%s,R(%u));ctx.cr[0]=0;ctx.cr[1]=0;ctx.cr[2]=ok;ctx.cr[3]=ctx.so;}", eax(a, b).c_str(), d));
      return true;
    case Op::DCBZ: line(strprintf("std::memset(mem()+(%s&~127u),0,128);", eax(a, b).c_str())); return true;
    case Op::DCBST: case Op::DCBF: case Op::DCBT: case Op::DCBTST: case Op::DCBI: case Op::ICBI: case Op::DST:
    case Op::DSTST: case Op::DSS:
      return true;
    case Op::SYNC: case Op::EIEIO: line("std::atomic_thread_fence(std::memory_order_seq_cst);"); return true;
    // ---- FP loads/stores ----
    case Op::LFS: line(strprintf("F(%u)=rf32(%s);", d, ead(a, si).c_str())); return true;
    case Op::LFD: line(strprintf("F(%u)=rf64(%s);", d, ead(a, si).c_str())); return true;
    case Op::LFSU: line(strprintf("{u32 ea=%s;F(%u)=rf32(ea);R(%u)=ea;}", ead(a, si).c_str(), d, a)); return true;
    case Op::LFDU: line(strprintf("{u32 ea=%s;F(%u)=rf64(ea);R(%u)=ea;}", ead(a, si).c_str(), d, a)); return true;
    case Op::LFSX: line(strprintf("F(%u)=rf32(%s);", d, eax(a, b).c_str())); return true;
    case Op::LFDX: line(strprintf("F(%u)=rf64(%s);", d, eax(a, b).c_str())); return true;
    case Op::LFSUX: line(strprintf("{u32 ea=%s;F(%u)=rf32(ea);R(%u)=ea;}", eax(a, b).c_str(), d, a)); return true;
    case Op::LFDUX: line(strprintf("{u32 ea=%s;F(%u)=rf64(ea);R(%u)=ea;}", eax(a, b).c_str(), d, a)); return true;
    case Op::STFS: line(strprintf("wf32(%s,F(%u));", ead(a, si).c_str(), d)); return true;
    case Op::STFD: line(strprintf("wf64(%s,F(%u));", ead(a, si).c_str(), d)); return true;
    case Op::STFSU: line(strprintf("{u32 ea=%s;wf32(ea,F(%u));R(%u)=ea;}", ead(a, si).c_str(), d, a)); return true;
    case Op::STFDU: line(strprintf("{u32 ea=%s;wf64(ea,F(%u));R(%u)=ea;}", ead(a, si).c_str(), d, a)); return true;
    case Op::STFSX: line(strprintf("wf32(%s,F(%u));", eax(a, b).c_str(), d)); return true;
    case Op::STFDX: line(strprintf("wf64(%s,F(%u));", eax(a, b).c_str(), d)); return true;
    case Op::STFSUX: line(strprintf("{u32 ea=%s;wf32(ea,F(%u));R(%u)=ea;}", eax(a, b).c_str(), d, a)); return true;
    case Op::STFDUX: line(strprintf("{u32 ea=%s;wf64(ea,F(%u));R(%u)=ea;}", eax(a, b).c_str(), d, a)); return true;
    case Op::STFIWX: line(strprintf("w32(%s,u32(dbits(F(%u))));", eax(a, b).c_str(), d)); return true;
    // ---- FPU arithmetic ----
    case Op::FADDS: FD(strprintf("rs(F(%u)+F(%u))", a, b)); return true;
    case Op::FSUBS: FD(strprintf("rs(F(%u)-F(%u))", a, b)); return true;
    case Op::FMULS: FD(strprintf("rs(F(%u)*F(%u))", a, c)); return true;
    case Op::FDIVS: FD(strprintf("rs(F(%u)/F(%u))", a, b)); return true;
    case Op::FSQRTS: FD(strprintf("rs(std::sqrt(F(%u)))", b)); return true;
    case Op::FRES: FD(strprintf("rs(1.0/F(%u))", b)); return true;
    case Op::FMADDS: FD(strprintf("rs(std::fma(F(%u),F(%u),F(%u)))", a, c, b)); return true;
    case Op::FMSUBS: FD(strprintf("rs(std::fma(F(%u),F(%u),-F(%u)))", a, c, b)); return true;
    case Op::FNMADDS: FD(strprintf("rs(-std::fma(F(%u),F(%u),F(%u)))", a, c, b)); return true;
    case Op::FNMSUBS: FD(strprintf("rs(-std::fma(F(%u),F(%u),-F(%u)))", a, c, b)); return true;
    case Op::FADD: FD(strprintf("F(%u)+F(%u)", a, b)); return true;
    case Op::FSUB: FD(strprintf("F(%u)-F(%u)", a, b)); return true;
    case Op::FMUL: FD(strprintf("F(%u)*F(%u)", a, c)); return true;
    case Op::FDIV: FD(strprintf("F(%u)/F(%u)", a, b)); return true;
    case Op::FSQRT: FD(strprintf("std::sqrt(F(%u))", b)); return true;
    case Op::FRSQRTE: FD(strprintf("1.0/std::sqrt(F(%u))", b)); return true;
    case Op::FSEL: FD(strprintf("fsel(F(%u),F(%u),F(%u))", a, b, c)); return true;
    case Op::FMADD: FD(strprintf("std::fma(F(%u),F(%u),F(%u))", a, c, b)); return true;
    case Op::FMSUB: FD(strprintf("std::fma(F(%u),F(%u),-F(%u))", a, c, b)); return true;
    case Op::FNMADD: FD(strprintf("-std::fma(F(%u),F(%u),F(%u))", a, c, b)); return true;
    case Op::FNMSUB: FD(strprintf("-std::fma(F(%u),F(%u),-F(%u))", a, c, b)); return true;
    case Op::FRSP: FD(strprintf("rs(F(%u))", b)); return true;
    case Op::FCTIW: FD(strprintf("fctiw(F(%u),false)", b)); return true;
    case Op::FCTIWZ: FD(strprintf("fctiw(F(%u),true)", b)); return true;
    case Op::FCTID: FD(strprintf("fctid(F(%u),false)", b)); return true;
    case Op::FCTIDZ: FD(strprintf("fctid(F(%u),true)", b)); return true;
    case Op::FCFID: FD(strprintf("fcfid(F(%u))", b)); return true;
    case Op::FMR: FD(strprintf("F(%u)", b)); return true;
    case Op::FNEG: FD(strprintf("-F(%u)", b)); return true;
    case Op::FABS: FD(strprintf("std::fabs(F(%u))", b)); return true;
    case Op::FNABS: FD(strprintf("-std::fabs(F(%u))", b)); return true;
    case Op::FCMPU: case Op::FCMPO: line(strprintf("fcmp(ctx,%u,F(%u),F(%u));", crfd(w), a, b)); return true;
    case Op::MFFS: FD("bitsd(u64(ctx.fpscr))"); return true;
    case Op::MTFSF: line(strprintf("mtfsf(ctx,0x%x,F(%u));", fm(w), b) + (rec ? "cr1_fp(ctx);" : "")); return true;
    case Op::MTFSFI: {
      u32 sh = 28 - 4 * crfd(w), imm = (w >> 12) & 15;
      line(strprintf("ctx.fpscr=(ctx.fpscr&~0x%xu)|0x%xu;", 0xFu << sh, imm << sh) + (rec ? "cr1_fp(ctx);" : ""));
      return true;
    }
    case Op::MTFSB0: line(strprintf("ctx.fpscr&=~0x%xu;", 1u << (31 - d)) + (rec ? "cr1_fp(ctx);" : "")); return true;
    case Op::MTFSB1: line(strprintf("ctx.fpscr|=0x%xu;", 1u << (31 - d)) + (rec ? "cr1_fp(ctx);" : "")); return true;
    case Op::MCRFS: {
      u32 sf = crfs(w), df = crfd(w);
      line(strprintf("for(int i=0;i<4;++i)ctx.cr[%u+i]=(ctx.fpscr>>(%u-i))&1;", 4 * df, 31 - 4 * sf));
      return true;
    }
    // ---- VMX loads/stores ----
    case Op::LVX: case Op::LVXL: case Op::LVEBX: case Op::LVEHX: case Op::LVEWX:
      line(strprintf("V(%u)=lvx(%s);", d, eax(a, b).c_str()));
      return true;
    case Op::STVX: case Op::STVXL: line(strprintf("stvx(%s,V(%u));", eax(a, b).c_str(), d)); return true;
    case Op::STVEBX: line(strprintf("stvebx(%s,V(%u));", eax(a, b).c_str(), d)); return true;
    case Op::STVEHX: line(strprintf("stvehx(%s,V(%u));", eax(a, b).c_str(), d)); return true;
    case Op::STVEWX: line(strprintf("stvewx(%s,V(%u));", eax(a, b).c_str(), d)); return true;
    case Op::LVSL: line(strprintf("V(%u)=lvsl(%s);", d, eax(a, b).c_str())); return true;
    case Op::LVSR: line(strprintf("V(%u)=lvsr(%s);", d, eax(a, b).c_str())); return true;
    case Op::LVLX: case Op::LVLXL: line(strprintf("V(%u)=lvlx(%s);", d, eax(a, b).c_str())); return true;
    case Op::LVRX: case Op::LVRXL: line(strprintf("V(%u)=lvrx(%s);", d, eax(a, b).c_str())); return true;
    case Op::STVLX: case Op::STVLXL: line(strprintf("stvlx(%s,V(%u));", eax(a, b).c_str(), d)); return true;
    case Op::STVRX: case Op::STVRXL: line(strprintf("stvrx(%s,V(%u));", eax(a, b).c_str(), d)); return true;
    // ---- VMX arithmetic ----
    case Op::VADDUBM: VI("_mm_add_epi8"); return true;
    case Op::VADDUHM: VI("_mm_add_epi16"); return true;
    case Op::VADDUWM: VI("_mm_add_epi32"); return true;
    case Op::VSUBUBM: VI("_mm_sub_epi8"); return true;
    case Op::VSUBUHM: VI("_mm_sub_epi16"); return true;
    case Op::VSUBUWM: VI("_mm_sub_epi32"); return true;
    case Op::VADDUBS: VI("_mm_adds_epu8"); return true;
    case Op::VADDUHS: VI("_mm_adds_epu16"); return true;
    case Op::VADDSBS: VI("_mm_adds_epi8"); return true;
    case Op::VADDSHS: VI("_mm_adds_epi16"); return true;
    case Op::VSUBUBS: VI("_mm_subs_epu8"); return true;
    case Op::VSUBUHS: VI("_mm_subs_epu16"); return true;
    case Op::VSUBSBS: VI("_mm_subs_epi8"); return true;
    case Op::VSUBSHS: VI("_mm_subs_epi16"); return true;
    case Op::VADDUWS: V2("vadduws"); return true;
    case Op::VADDSWS: V2("vaddsws"); return true;
    case Op::VSUBUWS: V2("vsubuws"); return true;
    case Op::VSUBSWS: V2("vsubsws"); return true;
    case Op::VADDCUW: V2("vaddcuw"); return true;
    case Op::VSUBCUW: V2("vsubcuw"); return true;
    case Op::VMAXUB: VI("_mm_max_epu8"); return true;
    case Op::VMAXUH: VI("_mm_max_epu16"); return true;
    case Op::VMAXUW: VI("_mm_max_epu32"); return true;
    case Op::VMAXSB: VI("_mm_max_epi8"); return true;
    case Op::VMAXSH: VI("_mm_max_epi16"); return true;
    case Op::VMAXSW: VI("_mm_max_epi32"); return true;
    case Op::VMINUB: VI("_mm_min_epu8"); return true;
    case Op::VMINUH: VI("_mm_min_epu16"); return true;
    case Op::VMINUW: VI("_mm_min_epu32"); return true;
    case Op::VMINSB: VI("_mm_min_epi8"); return true;
    case Op::VMINSH: VI("_mm_min_epi16"); return true;
    case Op::VMINSW: VI("_mm_min_epi32"); return true;
    case Op::VAVGUB: VI("_mm_avg_epu8"); return true;
    case Op::VAVGUH: VI("_mm_avg_epu16"); return true;
    case Op::VAVGUW: V2("vavguw"); return true;
    case Op::VAVGSB: V2("vavgsb"); return true;
    case Op::VAVGSH: V2("vavgsh"); return true;
    case Op::VAVGSW: V2("vavgsw"); return true;
    case Op::VAND: VI("_mm_and_si128"); return true;
    case Op::VANDC: VD(strprintf("mk(_mm_andnot_si128(V(%u).vi,V(%u).vi))", b, a)); return true;
    case Op::VOR: VI("_mm_or_si128"); return true;
    case Op::VXOR: VI("_mm_xor_si128"); return true;
    case Op::VNOR: VD(strprintf("mk(_mm_xor_si128(_mm_or_si128(V(%u).vi,V(%u).vi),_mm_set1_epi32(-1)))", a, b)); return true;
    case Op::VSEL: VD(strprintf("vsel(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VPERM: VD(strprintf("vperm(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VSLDOI: VD(strprintf("vsldoi(V(%u),V(%u),%u)", a, b, vsh(w))); return true;
    case Op::VRLB: VD(strprintf("vshift<u8,16>(V(%u),V(%u),3)", a, b)); return true;
    case Op::VRLH: VD(strprintf("vshift<u16,8>(V(%u),V(%u),3)", a, b)); return true;
    case Op::VRLW: V2("vrlw"); return true;
    case Op::VSLB: VD(strprintf("vshift<u8,16>(V(%u),V(%u),0)", a, b)); return true;
    case Op::VSLH: VD(strprintf("vshift<u16,8>(V(%u),V(%u),0)", a, b)); return true;
    case Op::VSLW: V2("vslw"); return true;
    case Op::VSRB: VD(strprintf("vshift<u8,16>(V(%u),V(%u),1)", a, b)); return true;
    case Op::VSRH: VD(strprintf("vshift<u16,8>(V(%u),V(%u),1)", a, b)); return true;
    case Op::VSRW: V2("vsrw"); return true;
    case Op::VSRAB: VD(strprintf("vshift<u8,16>(V(%u),V(%u),2)", a, b)); return true;
    case Op::VSRAH: VD(strprintf("vshift<u16,8>(V(%u),V(%u),2)", a, b)); return true;
    case Op::VSRAW: V2("vsraw"); return true;
    case Op::VSL: V2("vsl"); return true;
    case Op::VSR: V2("vsr"); return true;
    case Op::VSLO: V2("vslo"); return true;
    case Op::VSRO: V2("vsro"); return true;
    case Op::VMRGHB: V2("vmrghb"); return true;
    case Op::VMRGHH: V2("vmrghh"); return true;
    case Op::VMRGHW: V2("vmrghw"); return true;
    case Op::VMRGLB: V2("vmrglb"); return true;
    case Op::VMRGLH: V2("vmrglh"); return true;
    case Op::VMRGLW: V2("vmrglw"); return true;
    case Op::VPKUHUM: V2("vpkuhum"); return true;
    case Op::VPKUWUM: V2("vpkuwum"); return true;
    case Op::VPKUHUS: V2("vpkuhus"); return true;
    case Op::VPKUWUS: V2("vpkuwus"); return true;
    case Op::VPKSHSS: V2("vpkshss"); return true;
    case Op::VPKSHUS: V2("vpkshus"); return true;
    case Op::VPKSWSS: V2("vpkswss"); return true;
    case Op::VPKSWUS: V2("vpkswus"); return true;
    case Op::VPKPX: V2("vpkpx"); return true;
    case Op::VUPKHSB: VD(strprintf("vupkhsb(V(%u))", b)); return true;
    case Op::VUPKLSB: VD(strprintf("vupklsb(V(%u))", b)); return true;
    case Op::VUPKHSH: VD(strprintf("vupkhsh(V(%u))", b)); return true;
    case Op::VUPKLSH: VD(strprintf("vupklsh(V(%u))", b)); return true;
    case Op::VUPKHPX: VD(strprintf("vupkpx(V(%u),true)", b)); return true;
    case Op::VUPKLPX: VD(strprintf("vupkpx(V(%u),false)", b)); return true;
    case Op::VSPLTB: VD(strprintf("vspltb(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VSPLTH: VD(strprintf("vsplth(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VSPLTW: VD(strprintf("vspltw(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VSPLTISB: VD(strprintf("mk(_mm_set1_epi8(%d))", vsimm(w))); return true;
    case Op::VSPLTISH: VD(strprintf("mk(_mm_set1_epi16(%d))", vsimm(w))); return true;
    case Op::VSPLTISW: VD(strprintf("mk(_mm_set1_epi32(%d))", vsimm(w))); return true;
    case Op::VMULEUB: V2("vmuleub"); return true;
    case Op::VMULOUB: V2("vmuloub"); return true;
    case Op::VMULESB: V2("vmulesb"); return true;
    case Op::VMULOSB: V2("vmulosb"); return true;
    case Op::VMULEUH: V2("vmuleuh"); return true;
    case Op::VMULOUH: V2("vmulouh"); return true;
    case Op::VMULESH: V2("vmulesh"); return true;
    case Op::VMULOSH: V2("vmulosh"); return true;
    case Op::VMSUMUBM: VD(strprintf("vmsumubm(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VMSUMMBM: VD(strprintf("vmsummbm(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VMSUMUHM: VD(strprintf("vmsumuhm(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VMSUMUHS: VD(strprintf("vmsumuhs(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VMSUMSHM: VD(strprintf("vmsumshm(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VMSUMSHS: VD(strprintf("vmsumshs(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VMHADDSHS: VD(strprintf("vmhaddshs(V(%u),V(%u),V(%u),false)", a, b, c)); return true;
    case Op::VMHRADDSHS: VD(strprintf("vmhaddshs(V(%u),V(%u),V(%u),true)", a, b, c)); return true;
    case Op::VMLADDUHM: VD(strprintf("vmladduhm(V(%u),V(%u),V(%u))", a, b, c)); return true;
    case Op::VSUM4UBS: V2("vsum4ubs"); return true;
    case Op::VSUM4SBS: V2("vsum4sbs"); return true;
    case Op::VSUM4SHS: V2("vsum4shs"); return true;
    case Op::VSUM2SWS: V2("vsum2sws"); return true;
    case Op::VSUMSWS: V2("vsumsws"); return true;
    case Op::VADDFP: VF("_mm_add_ps"); return true;
    case Op::VSUBFP: VF("_mm_sub_ps"); return true;
    case Op::VMADDFP: VD(strprintf("mkf(_mm_fmadd_ps(V(%u).vf,V(%u).vf,V(%u).vf))", a, c, b)); return true;
    case Op::VNMSUBFP: VD(strprintf("mkf(_mm_fnmadd_ps(V(%u).vf,V(%u).vf,V(%u).vf))", a, c, b)); return true;
    case Op::VMAXFP: V2("vmaxfp"); return true;
    case Op::VMINFP: V2("vminfp"); return true;
    case Op::VREFP: VD(strprintf("vrefp(V(%u))", b)); return true;
    case Op::VRSQRTEFP: VD(strprintf("vrsqrtefp(V(%u))", b)); return true;
    case Op::VEXPTEFP: VD(strprintf("vexptefp(V(%u))", b)); return true;
    case Op::VLOGEFP: VD(strprintf("vlogefp(V(%u))", b)); return true;
    case Op::VRFIN: VD(strprintf("mkf(_mm_round_ps(V(%u).vf,_MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC))", b)); return true;
    case Op::VRFIZ: VD(strprintf("mkf(_mm_round_ps(V(%u).vf,_MM_FROUND_TO_ZERO|_MM_FROUND_NO_EXC))", b)); return true;
    case Op::VRFIP: VD(strprintf("mkf(_mm_round_ps(V(%u).vf,_MM_FROUND_TO_POS_INF|_MM_FROUND_NO_EXC))", b)); return true;
    case Op::VRFIM: VD(strprintf("mkf(_mm_round_ps(V(%u).vf,_MM_FROUND_TO_NEG_INF|_MM_FROUND_NO_EXC))", b)); return true;
    case Op::VCFSX: VD(strprintf("vcfsx(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VCFUX: VD(strprintf("vcfux(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VCTSXS: VD(strprintf("vctsxs(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VCTUXS: VD(strprintf("vctuxs(V(%u),%u)", b, vuimm(w))); return true;
    case Op::VCMPEQUB: VCMP(strprintf("mk(_mm_cmpeq_epi8(V(%u).vi,V(%u).vi))", a, b)); return true;
    case Op::VCMPEQUH: VCMP(strprintf("mk(_mm_cmpeq_epi16(V(%u).vi,V(%u).vi))", a, b)); return true;
    case Op::VCMPEQUW: VCMP(strprintf("mk(_mm_cmpeq_epi32(V(%u).vi,V(%u).vi))", a, b)); return true;
    case Op::VCMPGTSB: VCMP(strprintf("mk(_mm_cmpgt_epi8(V(%u).vi,V(%u).vi))", a, b)); return true;
    case Op::VCMPGTSH: VCMP(strprintf("mk(_mm_cmpgt_epi16(V(%u).vi,V(%u).vi))", a, b)); return true;
    case Op::VCMPGTSW: VCMP(strprintf("mk(_mm_cmpgt_epi32(V(%u).vi,V(%u).vi))", a, b)); return true;
    case Op::VCMPGTUB:
      VCMP(strprintf("mk(_mm_cmpgt_epi8(_mm_xor_si128(V(%u).vi,_mm_set1_epi8(-128)),_mm_xor_si128(V(%u).vi,_mm_set1_epi8(-128))))", a, b));
      return true;
    case Op::VCMPGTUH:
      VCMP(strprintf("mk(_mm_cmpgt_epi16(_mm_xor_si128(V(%u).vi,_mm_set1_epi16(-32768)),_mm_xor_si128(V(%u).vi,_mm_set1_epi16(-32768))))", a, b));
      return true;
    case Op::VCMPGTUW:
      VCMP(strprintf("mk(_mm_cmpgt_epi32(_mm_xor_si128(V(%u).vi,_mm_set1_epi32(INT32_MIN)),_mm_xor_si128(V(%u).vi,_mm_set1_epi32(INT32_MIN))))", a, b));
      return true;
    case Op::VCMPEQFP: VCMP(strprintf("mkf(_mm_cmpeq_ps(V(%u).vf,V(%u).vf))", a, b)); return true;
    case Op::VCMPGEFP: VCMP(strprintf("mkf(_mm_cmpge_ps(V(%u).vf,V(%u).vf))", a, b)); return true;
    case Op::VCMPGTFP: VCMP(strprintf("mkf(_mm_cmpgt_ps(V(%u).vf,V(%u).vf))", a, b)); return true;
    case Op::VCMPBFP:
      line(strprintf("V(%u)=vcmpbfp(V(%u),V(%u));", d, a, b) + (vrc(w) ? strprintf("cr6_bounds(ctx,V(%u));", d) : ""));
      return true;
    case Op::MFVSCR: VD("mk(_mm_setr_epi32(int(ctx.vscr),0,0,0))"); return true;
    case Op::MTVSCR: line(strprintf("ctx.vscr=V(%u).u32x[0];", b)); return true;
    default: break;
  }
  line(strprintf("trap(ctx,0x%08xu,\"unimplemented %s\");", pc, op_name(op)));
  return false;
}

void FnEmitter::body(const std::string& hook) {
  // A hooked function's guest code gets its own name; f_<entry> forwards to the runtime hook.
  if (!hook.empty())
    out += strprintf("namespace ppu { void hook_%s(Ctx&, void (*)(Ctx&)); }\nvoid f_%08x_guest(Ctx& __restrict ctx);\n"
                     "void f_%08x(Ctx& __restrict ctx){ ppu::hook_%s(ctx, f_%08x_guest); }\n",
                     hook.c_str(), f.entry, f.entry, hook.c_str(), f.entry);
  out += strprintf(hook.empty() ? "void f_%08x(Ctx& __restrict ctx){\n" : "void f_%08x_guest(Ctx& __restrict ctx){\n", f.entry);
  const auto& v = f.insns;
  for (size_t i = 0; i < v.size(); ++i) {
    u32 pc = v[i];
    if (f.labels.count(pc)) out += strprintf("L_%08x:\n", pc);
    u32 w = img.r32(pc);
    if (comments) {
      char buf[128];
      disasm(pc, w, buf, sizeof buf);
      out += strprintf("  // %08x: %s\n", pc, buf);
    }
    bool ft = insn(pc, w);
    if (ft) {
      bool next_is_succ = i + 1 < v.size() && v[i + 1] == pc + 4;
      if (!next_is_succ) line(jump(pc + 4));
    }
  }
  out += "}\n";
}

void write_if_changed(const std::filesystem::path& p, const std::string& s, size_t& written) {
  if (std::filesystem::exists(p)) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    if (ss.str() == s) return;
  }
  ac4::write_text(p, s);
  ++written;
}

const char* kPrelude =
    "// Generated by ac4-recomp from the user's own game executable. Game-derived: never\n"
    "// commit or distribute this file.\n"
    "#include \"ppu/ppu_recomp.h\"\n"
    "#pragma clang diagnostic ignored \"-Wunused-label\"\n"
    "#pragma clang diagnostic ignored \"-Wunused-variable\"\n"
    "using namespace ppu;\n"
    "#define R(n) ctx.r[n]\n#define F(n) ctx.f[n]\n#define V(n) ctx.v[n]\n";

}  // namespace

void emit_all(const std::vector<Unit>& units, const std::string& out_dir, const EmitOptions& opt) {
  namespace fs = std::filesystem;
  fs::path out(out_dir);
  ac4::guard_write(out);
  fs::create_directories(out);
  size_t written = 0, total_files = 0, max_fn = 0;
  u32 max_fn_at = 0;
  std::string info;
  for (const Unit& u : units) {
    size_t file_idx = 0, in_file = 0;
    std::string bodies;
    std::set<u32> refs;
    auto flush = [&]() {
      if (bodies.empty()) return;
      std::string s = kPrelude;
      for (u32 r : refs) s += ac4::strprintf("void f_%08x(Ctx&);\n", r);
      s += bodies;
      write_if_changed(out / ac4::strprintf("%s_gen_%03zu.cpp", u.prefix.c_str(), file_idx), s, written);
      ++file_idx;
      bodies.clear();
      refs.clear();
      in_file = 0;
    };
    for (const auto& [e, f] : u.an->functions()) {
      FnEmitter fe{*u.img, *u.an, f, refs, opt.comments, {}};
      auto hk = opt.hooks.find(e);
      fe.body(hk == opt.hooks.end() ? std::string() : hk->second);
      bodies += fe.out;
      in_file += f.insns.size();
      if (f.insns.size() > max_fn) {
        max_fn = f.insns.size();
        max_fn_at = e;
      }
      if (in_file >= opt.insns_per_file) flush();
    }
    flush();
    for (size_t i = file_idx;; ++i) {  // remove stale chunks from a previous, larger emission
      fs::path p = out / ac4::strprintf("%s_gen_%03zu.cpp", u.prefix.c_str(), i);
      if (!fs::exists(p)) break;
      fs::remove(p);
    }
    total_files += file_idx;
    const auto& st = u.an->stats();
    info += ac4::strprintf("[%s] functions %zu (secondary %zu) insns %zu switches %zu unresolved_bctr %zu invalid %zu files %zu\n",
                            u.prefix.c_str(), st.functions, st.secondary, st.insns, st.switches, st.unresolved_bctr, st.invalid,
                            file_idx);
  }
  // Function table over all units.
  std::string t = kPrelude;
  size_t count = 0;
  for (const Unit& u : units)
    for (const auto& [e, f] : u.an->functions()) {
      t += ac4::strprintf("void f_%08x(Ctx&);\n", e);
      ++count;
    }
  t += "namespace ppu { struct FuncEntry { u32 addr; Fn fn; };\nextern const FuncEntry g_funcs[];\nextern const size_t g_func_count;\n";
  t += "const FuncEntry g_funcs[] = {\n";
  for (const Unit& u : units)
    for (const auto& [e, f] : u.an->functions()) t += ac4::strprintf("{0x%08xu,f_%08x},\n", e, e);
  t += ac4::strprintf("};\nconst size_t g_func_count = %zu;\n}\n", count);
  write_if_changed(out / "ppu_functab.cpp", t, written);
  info += ac4::strprintf("largest_function 0x%08x (%zu insns), files %zu\n", max_fn_at, max_fn, total_files);
  write_if_changed(out / "gen_info.txt", info, written);
  std::printf("%s%zu file(s) written to %s\n", info.c_str(), written, out_dir.c_str());
}

}  // namespace recomp