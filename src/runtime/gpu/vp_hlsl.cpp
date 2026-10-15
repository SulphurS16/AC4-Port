// RSX vertex program microcode -> HLSL. Field layouts and semantics: rsx_facts.md §1.
#include "runtime/gpu/vp_hlsl.h"

#include <algorithm>
#include <set>
#include <vector>

namespace rt::gpu {
namespace {

// Attribute fetch from raw big-endian guest bytes (vp_hlsl.h VtxFmt); the conversions are the
// renderer's CPU ones (convert_attr: S16 normalized (x + 0.5) / 32767.5, CMP 11:11:10 / 32767).
// Kept small: the program calls it once, in a loop over its inputs (inlined per input it made
// vs_5_0 compiles take 2-4 s instead of ~0.2 s).
const char* const kVertexFetchHlsl = R"(ByteAddressBuffer vb0 : register(t0);
ByteAddressBuffer vb1 : register(t1);
ByteAddressBuffer vb2 : register(t2);
ByteAddressBuffer vb3 : register(t3);
cbuffer VtxFmt : register(b2) { uint4 va[16]; float4 vk[16]; };
uint4 bswap32(uint4 x) { return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24); }
uint4 vb_load(uint b, uint a) {
  uint q = a & ~3u, s = (a & 3) * 8;
  uint4 w;
  uint x;
  [branch] switch (b) {
    case 0: w = vb0.Load4(q); x = vb0.Load(q + 16); break;
    case 1: w = vb1.Load4(q); x = vb1.Load(q + 16); break;
    case 2: w = vb2.Load4(q); x = vb2.Load(q + 16); break;
    default: w = vb3.Load4(q); x = vb3.Load(q + 16); break;
  }
  return s == 0 ? w : (w >> s) | (uint4(w.yzw, x) << (32 - s));
}
float4 fetch(uint i, uint k) {
  uint4 d = va[i];
  if (((d.w >> 16) & 1) == 0) return vk[i];
  uint type = d.w & 15, size = (d.w >> 4) & 15;
  uint kk = k;
  if (d.z > 1) kk = ((d.w >> 12) & 1) ? k % d.z : k / d.z;
  if (d.y == 0) kk = 0;
  uint4 e = vb_load((d.w >> 8) & 3, d.x + kk * d.y);
  float4 f;
  [branch] switch (type) {
    case 2:
      f = asfloat(bswap32(e));
      break;
    case 1:
    case 3:
    case 5: {
      uint4 h = uint4(e.x, e.x >> 16, e.y, e.y >> 16);
      h = ((h & 0xff) << 8) | ((h >> 8) & 0xff);
      f = type == 3 ? f16tof32(h) : float4(int4(h << 16) >> 16);
      if (type == 1) f = (f + 0.5) / 32767.5;
      break;
    }
    case 6: {
      uint v = bswap32(e).x;
      f = float4(float3(int3(uint3((v & 0x7ff) << 21, ((v >> 11) & 0x7ff) << 21, ((v >> 22) & 0x3ff) << 22)) >> 16) / 32767.0, 1);
      size = 3;
      break;
    }
    default:
      f = float4((e.xxxx >> uint4(0, 8, 16, 24)) & 0xff);
      if (type == 4) f /= 255.0;
      break;
  }
  return float4(size > 0 ? f.x : 0, size > 1 ? f.y : 0, size > 2 ? f.z : 0, size > 3 ? f.w : 1);
}
)";

enum VecOp : u32 {
  V_NOP, V_MOV, V_MUL, V_ADD, V_MAD, V_DP3, V_DPH, V_DP4, V_DST, V_MIN, V_MAX, V_SLT, V_SGE, V_ARL, V_FRC, V_FLR,
  V_SEQ, V_SFL, V_SGT, V_SLE, V_SNE, V_STR, V_SSG, V_TXL = 0x19,
};
enum ScaOp : u32 {
  S_NOP, S_MOV, S_RCP, S_RCC, S_RSQ, S_EXP, S_LOG, S_LIT, S_BRA, S_BRI, S_CAL, S_CLI, S_RET, S_LG2, S_EX2, S_SIN, S_COS,
  S_BRB, S_CLB, S_PSH, S_POP,
};

struct Insn {
  u32 d0, d1, d2, d3;
  u32 vec_op() const { return (d1 >> 22) & 0x1f; }
  u32 sca_op() const { return (d1 >> 27) & 0x1f; }
  bool end() const { return d3 & 1; }
  u32 cond() const { return (d0 >> 10) & 7; }
  bool cond_test() const { return (d0 >> 13) & 1; }
  bool cond_update() const { return ((d0 >> 14) & 1) || ((d0 >> 29) & 1); }
  u32 vec_dst_tmp() const { return (d0 >> 15) & 0x3f; }
  bool abs(int n) const { return (d0 >> (21 + n)) & 1; }
  u32 addr_reg() const { return (d0 >> 24) & 1; }
  u32 cc_reg() const { return (d0 >> 25) & 1; }
  bool saturate() const { return (d0 >> 26) & 1; }
  bool vec_result() const { return (d0 >> 30) & 1; }
  u32 addr_swz() const { return d0 & 3; }
  u32 cc_swz(int c) const { return (d0 >> (8 - 2 * c)) & 3; }  // c: 0 x, 1 y, 2 z, 3 w
  u32 input_src() const { return (d1 >> 8) & 0xf; }
  u32 const_src() const { return (d1 >> 12) & 0x3ff; }
  bool index_const() const { return (d3 >> 1) & 1; }
  u32 dst() const { return (d3 >> 2) & 0x1f; }
  u32 sca_dst_tmp() const { return (d3 >> 7) & 0x3f; }
  u32 vec_mask() const { return (d3 >> 13) & 0xf; }  // bit3 x, bit2 y, bit1 z, bit0 w
  u32 sca_mask() const { return (d3 >> 17) & 0xf; }
  u32 src(int n) const {
    switch (n) {
      case 0: return ((d2 >> 23) & 0x1ff) | ((d1 & 0xff) << 9);
      case 1: return (d2 >> 6) & 0x1ffff;
      default: return ((d3 >> 21) & 0x7ff) | ((d2 & 0x3f) << 11);
    }
  }
  u32 branch_target() const { return (((d0 >> 23) & 1) << 9) | ((d2 & 0x3f) << 3) | ((d3 >> 29) & 7); }
  u32 branch_bit() const { return (d3 >> 23) & 0x1f; }
  bool branch_bit_true() const { return (d3 >> 28) & 1; }
};

const char kComp[] = "xyzw";

Insn load(const u32* slots, u32 pc) { return {slots[4 * pc], slots[4 * pc + 1], slots[4 * pc + 2], slots[4 * pc + 3]}; }

class Emitter {
 public:
  Emitter(VpInfo* info) : info_(info) {}

  std::string src(const Insn& in, int n) {
    u32 s = in.src(n);
    std::string reg;
    switch (s & 3) {
      case 1: {
        u32 r = (s >> 2) & 0x3f;
        max_r_ = std::max(max_r_, r);
        reg = "r[" + std::to_string(r) + "]";
        break;
      }
      case 2:
        info_->input_mask |= 1u << in.input_src();
        reg = "v[" + std::to_string(in.input_src()) + "]";
        break;
      case 3:
        if (in.index_const())
          reg = "vc[(a[" + std::to_string(in.addr_reg()) + "]." + kComp[in.addr_swz()] + " + " +
                std::to_string(in.const_src()) + ") & 511]";
        else
          reg = "vc[" + std::to_string(in.const_src()) + "]";
        break;
      default:
        if (info_->error.empty()) info_->error = "invalid source register type 0";
        reg = "float4(0, 0, 0, 0)";
        break;
    }
    std::string sw;
    sw += kComp[(s >> 14) & 3];
    sw += kComp[(s >> 12) & 3];
    sw += kComp[(s >> 10) & 3];
    sw += kComp[(s >> 8) & 3];
    std::string e = "(" + reg + ")." + sw;
    if (in.abs(n)) e = "abs(" + e + ")";
    if ((s >> 16) & 1) e = "(-" + e + ")";
    return e;
  }

  // Masked (and possibly condition-tested) write of `val` to `dst`.
  std::string write(const Insn& in, const std::string& dst, const std::string& val, u32 mask) {
    std::string out;
    if (!in.cond_test()) {
      std::string m;
      for (int c = 0; c < 4; ++c)
        if (mask & (8 >> c)) m += kComp[c];
      if (!m.empty()) out += dst + "." + m + " = " + val + "." + m + "; ";
      return out;
    }
    std::string ccr = "cc[" + std::to_string(in.cc_reg()) + "].";
    for (int c = 0; c < 4; ++c) {
      if (!(mask & (8 >> c))) continue;
      out += "if (" + cond_expr(ccr + kComp[in.cc_swz(c)], in.cond()) + ") " + dst + "." + kComp[c] + " = " + val + "." +
             kComp[c] + "; ";
    }
    return out;
  }

  static std::string cond_expr(const std::string& v, u32 cond) {
    if (cond == 7) return "true";
    if (cond == 0) return "false";
    std::string e;
    auto add = [&](const char* t) { e += (e.empty() ? "" : " || ") + std::string("(") + v + t + ")"; };
    if (cond & 1) add(" < 0");
    if (cond & 2) add(" == 0");
    if (cond & 4) add(" > 0");
    return "(" + e + ")";
  }

  // Branch condition: the CC test on the x swizzle component.
  std::string branch_cond(const Insn& in) {
    if (in.sca_op() == S_BRB || in.sca_op() == S_CLB)
      return std::string("(((vp_flags.x >> ") + std::to_string(in.branch_bit()) + ") & 1) == " +
             (in.branch_bit_true() ? "1" : "0") + ")";
    return cond_expr("cc[" + std::to_string(in.cc_reg()) + "]." + kComp[in.cc_swz(0)], in.cond());
  }

  std::string vec_expr(const Insn& in, u32 op) {
    switch (op) {
      case V_MOV: return "s0";
      case V_MUL: return "s0 * s1";
      case V_ADD: return "s0 + s2";  // the VP adds src0 and src2
      case V_MAD: return "s0 * s1 + s2";
      case V_DP3: return "dot(s0.xyz, s1.xyz).xxxx";
      case V_DPH: return "(dot(s0.xyz, s1.xyz) + s1.w).xxxx";
      case V_DP4: return "dot(s0, s1).xxxx";
      case V_DST: return "float4(1, s0.y * s1.y, s0.z, s1.w)";
      case V_MIN: return "min(s0, s1)";
      case V_MAX: return "max(s0, s1)";
      case V_SLT: return "float4(s0 < s1)";
      case V_SGE: return "float4(s0 >= s1)";
      case V_FRC: return "frac(s0)";
      case V_FLR: return "floor(s0)";
      case V_SEQ: return "float4(s0 == s1)";
      case V_SFL: return "float4(0, 0, 0, 0)";
      case V_SGT: return "float4(s0 > s1)";
      case V_SLE: return "float4(s0 <= s1)";
      case V_SNE: return "float4(s0 != s1)";
      case V_STR: return "float4(1, 1, 1, 1)";
      case V_SSG: return "sign(s0)";
      case V_TXL:
        if (info_->error.empty()) info_->error = "vertex texture fetch (TXL) is not implemented";
        return "float4(0, 0, 0, 0)";
      default:
        if (info_->error.empty()) info_->error = "undefined vector opcode " + std::to_string(op);
        return "float4(0, 0, 0, 0)";
    }
  }

  std::string sca_expr(u32 op) {
    switch (op) {
      case S_MOV: return "s2.xxxx";
      case S_RCP: return "(1.0 / s2.x).xxxx";
      case S_RCC: return "rcc(s2.x).xxxx";
      case S_RSQ: return "rsqrt(abs(s2.x)).xxxx";
      case S_EXP: return "float4(exp2(floor(s2.x)), s2.x - floor(s2.x), exp2(s2.x), 1)";
      case S_LOG: return "vp_log(s2.x)";
      case S_LIT:
        return "float4(1, max(s2.x, 0), s2.x > 0 ? pow(max(s2.y, 1.17549e-38), clamp(s2.w, -128, 128)) : 0, 1)";
      case S_LG2: return "log2(abs(s2.x)).xxxx";
      case S_EX2: return "exp2(s2.x).xxxx";
      case S_SIN: return "sin(s2.x).xxxx";
      case S_COS: return "cos(s2.x).xxxx";
      default:
        if (info_->error.empty()) info_->error = "unsupported scalar opcode " + std::to_string(op);
        return "float4(0, 0, 0, 0)";
    }
  }

  // Data part of an instruction (vector + scalar ALU ops, not the branch).
  std::string alu(const Insn& in) {
    if (in.cond() == 0) return "";  // never executes
    u32 vop = in.vec_op(), sop = in.sca_op();
    bool sca_alu = sop != S_NOP && sop != S_BRA && sop != S_BRI && sop != S_CAL && sop != S_CLI && sop != S_RET &&
                   sop != S_BRB && sop != S_CLB;
    std::string out;
    if (vop != V_NOP) out += "s0 = " + src(in, 0) + "; s1 = " + src(in, 1) + "; ";
    if (vop != V_NOP || sca_alu) out += "s2 = " + src(in, 2) + "; ";
    std::string writes;
    if (vop == V_ARL) {
      writes += write(in, "a[" + std::to_string(in.vec_dst_tmp() & 1) + "]", "int4(floor(s0))", in.vec_mask());
    } else if (vop != V_NOP) {
      out += "vres = " + vec_expr(in, vop) + "; ";
      if (in.saturate()) out += "vres = saturate(vres); ";
      bool to_r = in.vec_dst_tmp() != 0x3f, to_o = in.vec_result() && in.dst() != 0x1f;
      if (to_r) {
        max_r_ = std::max(max_r_, in.vec_dst_tmp());
        writes += write(in, "r[" + std::to_string(in.vec_dst_tmp()) + "]", "vres", in.vec_mask());
      }
      if (to_o) {
        info_->output_mask |= 1u << in.dst();
        writes += write(in, "o[" + std::to_string(in.dst()) + "]", "vres", in.vec_mask());
      }
      if (!to_r && !to_o && in.cond_update())
        writes += write(in, "cc[" + std::to_string(in.cc_reg()) + "]", "vres", in.vec_mask());
    }
    if (sca_alu) {
      out += "sres = " + sca_expr(sop) + "; ";
      if (in.saturate()) out += "sres = saturate(sres); ";
      if (in.sca_dst_tmp() != 0x3f) {
        max_r_ = std::max(max_r_, in.sca_dst_tmp());
        writes += write(in, "r[" + std::to_string(in.sca_dst_tmp()) + "]", "sres", in.sca_mask());
      } else if (!in.vec_result() && in.dst() != 0x1f) {
        info_->output_mask |= 1u << in.dst();
        writes += write(in, "o[" + std::to_string(in.dst()) + "]", "sres", in.sca_mask());
      } else if (in.cond_update()) {
        writes += write(in, "cc[" + std::to_string(in.cc_reg()) + "]", "sres", in.sca_mask());
      }
    }
    if (sop == S_PSH || sop == S_POP) {
      if (info_->error.empty()) info_->error = "PSH/POP are not implemented";
    }
    return out + writes;
  }

  u32 max_r() const { return max_r_; }

 private:
  VpInfo* info_;
  u32 max_r_ = 0;
};

}  // namespace

std::string vp_to_hlsl(const u32* slots, u32 start, VpInfo* info) {
  *info = VpInfo{};
  start = std::min(start, kVpSlots - 1);
  // Reachable instructions (follow branches and calls; stop at END).
  std::set<u32> reach;
  std::vector<u32> work{start};
  bool branches = false;
  while (!work.empty()) {
    u32 pc = work.back();
    work.pop_back();
    while (pc < kVpSlots && !reach.count(pc)) {
      reach.insert(pc);
      Insn in = load(slots, pc);
      u32 sop = in.sca_op();
      bool stop = in.end();
      switch (sop) {
        case S_BRI:
        case S_BRB:
          branches = true;
          work.push_back(in.branch_target());
          if (sop == S_BRI && in.cond() == 7) stop = true;
          break;
        case S_CAL:
        case S_CLI:
        case S_CLB:
          branches = true;
          work.push_back(in.branch_target());
          break;
        case S_RET:
          branches = true;
          if (in.cond() == 7) stop = true;
          break;
        case S_BRA:
          branches = true;
          if (info->error.empty()) info->error = "indirect branch (BRA) is not implemented";
          break;
        default:
          break;
      }
      if (stop) break;
      ++pc;
    }
  }
  if (reach.empty()) {
    info->error = "empty vertex program";
    return "";
  }
  info->first = *reach.begin();
  info->last = *reach.rbegin();
  info->branches = branches;

  Emitter em(info);
  std::string body;
  if (!branches) {
    for (u32 pc = start; pc < kVpSlots; ++pc) {
      Insn in = load(slots, pc);
      body += "  " + em.alu(in) + "\n";
      if (in.end()) break;
    }
  } else {
    body += "  int pc = " + std::to_string(start) + ", sp = 0; int stk[8] = {0,0,0,0,0,0,0,0};\n";
    body += "  [loop] for (int guard = 0; pc >= 0 && guard < 65536; ++guard) {\n    switch (pc) {\n";
    for (u32 pc : reach) {
      Insn in = load(slots, pc);
      body += "    case " + std::to_string(pc) + ": { " + em.alu(in);
      std::string next = in.end() ? "-1" : std::to_string(pc + 1);
      std::string cond = em.branch_cond(in);
      switch (in.sca_op()) {
        case S_BRI:
        case S_BRB:
          body += "pc = " + cond + " ? " + std::to_string(in.branch_target()) + " : " + next + "; ";
          break;
        case S_CAL:
        case S_CLI:
        case S_CLB:
          body += "if (" + cond + ") { if (sp < 8) stk[sp++] = " + std::to_string(pc + 1) + "; pc = " +
                  std::to_string(in.branch_target()) + "; } else pc = " + next + "; ";
          break;
        case S_RET:
          body += "if (" + cond + ") pc = sp > 0 ? stk[--sp] : -1; else pc = " + next + "; ";
          break;
        default:
          body += "pc = " + next + "; ";
          break;
      }
      body += "break; }\n";
    }
    body += "    default: pc = -1; break;\n    }\n  }\n";
  }

  std::string s;
  s += "cbuffer VpConst : register(b0) { float4 vc[512]; };\n";
  s += "cbuffer VpState : register(b1) { float4 vp_scale; float4 vp_offset; float4 rt_size; uint4 vp_flags; };\n";
  s += kVertexFetchHlsl;
  s += "struct VsOut { float4 pos : SV_Position;";
  for (int i = 0; i < 16; ++i) s += " float4 o" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";";
  s += " };\n";
  s += "float rcc(float x) { float r = 1.0 / x; return sign(r) * clamp(abs(r), 5.42101e-20, 1.884467e19); }\n";
  s += "float4 vp_log(float x) { float ax = abs(x); float e = floor(log2(ax)); return float4(e, ax / exp2(e), log2(ax), 1); }\n";
  s += "VsOut main(uint vid : SV_VertexID) {\n";
  s += "  float4 v[16];\n";
  s += "  [unroll] for (uint vi = 0; vi < 16; ++vi) v[vi] = float4(0, 0, 0, 1);\n";
  s += "  [loop] for (uint vm = " + std::to_string(info->input_mask) +
       "u; vm != 0; vm &= vm - 1) { uint vi = firstbitlow(vm); v[vi] = fetch(vi, vid); }\n";
  u32 nr = em.max_r() + 1;
  s += "  float4 r[" + std::to_string(nr) + "];\n  [unroll] for (int ri = 0; ri < " + std::to_string(nr) +
       "; ++ri) r[ri] = float4(0, 0, 0, 0);\n";
  s += "  float4 o[16];\n  [unroll] for (int oi = 0; oi < 16; ++oi) o[oi] = float4(0, 0, 0, 1);\n";
  s += "  float4 cc[2] = { float4(0, 0, 0, 0), float4(0, 0, 0, 0) };\n";
  s += "  int4 a[2] = { int4(0, 0, 0, 0), int4(0, 0, 0, 0) };\n";
  s += "  float4 s0 = 0, s1 = 0, s2 = 0, vres = 0, sres = 0;\n";
  s += body;
  // RSX viewport transform, then D3D NDC for the render target (y down in memory = y up in NDC).
  s += "  VsOut res;\n";
  s += "  float4 p = o[0];\n";
  s += "  float w = p.w == 0 ? 1e-20 : p.w;\n";
  s += "  float3 win = (p.xyz / w) * vp_scale.xyz + vp_offset.xyz;\n";
  s += "  res.pos = float4((win.x * rt_size.z * 2 - 1) * w, (1 - win.y * rt_size.w * 2) * w, win.z * w, w);\n";
  for (int i = 0; i < 16; ++i) s += "  res.o" + std::to_string(i) + " = o[" + std::to_string(i) + "];\n";
  s += "  return res;\n}\n";
  return s;
}

}  // namespace rt::gpu
