// RSX fragment program microcode -> HLSL. Field layouts and semantics: rsx_facts.md §2.
//
// Approximations (documented in docs/STATUS.md): H registers are separate from R registers (the
// hardware aliases H[2n]/H[2n+1] onto R[n] as packed fp16); fp16 results are not quantized.
#include "runtime/gpu/fp_hlsl.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>

namespace rt::gpu {
namespace {

enum Op : u32 {
  NOP = 0x00, MOV, MUL, ADD, MAD, DP3, DP4, DST, MIN, MAX, SLT, SGE, SLE, SGT, SNE, SEQ, FRC, FLR, KIL, PK4, UP4, DDX,
  DDY, TEX, TXP, TXD, RCP, RSQ, EX2, LG2, LIT, LRP, STR, SFL, COS, SIN, PK2, UP2, POW, PKB, UPB, PK16, UP16, BEM, PKG,
  UPG, DP2A, TXL, TXB = 0x31, TEXBEM = 0x33, TXPBEM, BEMLUM, REFL, TIMESWTEX, DP2, NRM, DIV, DIVSQ, LIF, FENCT, FENCB,
  BRK = 0x40, CAL, IFE, LOOP, REP, RET,
};

struct Slot {
  u32 w[4];
  bool is_const;
  u32 offset;  // bytes from the ucode start
};

const char kComp[] = "xyzw";

bool has_const(const u32* w) { return (w[1] & 3) == 2 || (w[2] & 3) == 2 || (w[3] & 3) == 2; }
u32 opcode(const u32* w) { return ((w[0] >> 24) & 0x3f) | (((w[2] >> 31) & 1) << 6); }

// Splits the ucode into 16-byte slots (instructions and inline constants) starting at the first
// non-NOP instruction. Returns false if no END marker was found.
bool scan(const u8* p, u32 max_bytes, std::vector<Slot>* slots, u32* size) {
  u32 off = 0;
  while (off + 16 <= max_bytes) {  // leading NOPs are skipped; offsets count from the first real one
    u32 w0 = fp_word(p + off);
    if (((w0 >> 24) & 0x3f) != 0) break;
    if (w0 & 1) {  // END on a leading NOP: empty program
      *size = off + 16;
      return true;
    }
    off += 16;
  }
  u32 base = off;
  while (off + 16 <= max_bytes) {
    Slot s{};
    for (int i = 0; i < 4; ++i) s.w[i] = fp_word(p + off + 4 * i);
    s.offset = off - base;
    slots->push_back(s);
    bool end = s.w[0] & 1;
    bool k = has_const(s.w);
    off += 16;
    if (k) {
      if (off + 16 > max_bytes) return false;
      Slot c{};
      for (int i = 0; i < 4; ++i) c.w[i] = fp_word(p + off + 4 * i);
      c.is_const = true;
      c.offset = off - base;
      slots->push_back(c);
      off += 16;
    }
    if (end) {
      *size = off;
      return true;
    }
    if (slots->size() > 16384) return false;
  }
  return false;
}

class Emitter {
 public:
  Emitter(const std::vector<Slot>& slots, const FpEnv& env, FpInfo* info) : slots_(slots), env_(env), info_(info) {
    for (size_t i = 0; i < slots.size(); ++i)
      if (slots[i].is_const) {
        const_index_[u32(i)] = u32(info->const_offsets.size());
        info->const_offsets.push_back(slots[i].offset);
      }
  }

  void fail(const std::string& e) {
    if (info_->error.empty()) info_->error = e;
  }

  std::string input(u32 attr, const u32* w) {
    std::string e;
    switch (attr) {
      case 0: return "wpos";
      case 1: return "col0";
      case 2: return "col1";
      case 3: return "fogc";
      case 14: return "ssa";
      case 15: fail("invalid input register 15"); return "float4(0, 0, 0, 0)";
      default: break;
    }
    if ((w[3] >> 30) & 1)  // indexed by the loop counter
      e = "tc[clamp(lc + " + std::to_string(attr - 4 + ((w[3] >> 19) & 0x7ff)) + ", 0, 9)]";
    else
      e = "tc[" + std::to_string(attr - 4) + "]";
    if ((w[3] >> 31) & 1) e = "(" + e + " * inv_w)";  // perspective correction
    return e;
  }

  static std::string prec_clamp(const std::string& e, u32 p) {
    switch (p) {
      case 1: return "clamp(" + e + ", -65504, 65504)";
      case 2: return "clamp(" + e + ", -2, 2)";
      case 3: return "clamp(" + e + ", -1, 1)";
      case 4: return "saturate(" + e + ")";
      default: return e;
    }
  }

  std::string src(u32 si, int n) {
    const u32* w = slots_[si].w;
    u32 s = w[1 + n];
    std::string base;
    bool color_in = false;
    switch (s & 3) {
      case 0: {
        u32 idx = (s >> 2) & 0x3f;
        if ((s >> 8) & 1) {
          max_h_ = std::max(max_h_, idx);
          base = "R.h[" + std::to_string(idx) + "]";
        } else {
          max_r_ = std::max(max_r_, idx);
          base = "R.r[" + std::to_string(idx) + "]";
        }
        break;
      }
      case 1: {
        u32 attr = (w[0] >> 13) & 0xf;
        color_in = attr == 1 || attr == 2;
        base = input(attr, w);
        break;
      }
      case 2: {
        auto it = const_index_.find(si + 1);
        if (it == const_index_.end()) {
          fail("constant operand without a constant slot");
          base = "float4(0, 0, 0, 0)";
        } else {
          base = "fc[" + std::to_string(it->second) + "]";
        }
        break;
      }
      default:
        fail("invalid source register type 3");
        base = "float4(0, 0, 0, 0)";
        break;
    }
    std::string sw;
    sw += kComp[(s >> 9) & 3];
    sw += kComp[(s >> 11) & 3];
    sw += kComp[(s >> 13) & 3];
    sw += kComp[(s >> 15) & 3];
    std::string e = "(" + base + ")." + sw;
    bool abs = n == 0 ? (w[1] >> 29) & 1 : n == 1 ? (w[2] >> 18) & 1 : (w[3] >> 18) & 1;
    u32 prec = (w[2] >> (19 + 3 * n)) & 7;
    if (color_in) {
      if (abs) e = "abs(" + e + ")";
    } else if (prec == 4) {
      e = "saturate(" + e + ")";  // saturate, then abs (a no-op on [0,1])
    } else {
      if (abs) e = "abs(" + e + ")";
      e = prec_clamp(e, prec);
    }
    if ((s >> 17) & 1) e = "(-" + e + ")";
    return e;
  }

  // Condition test of the instruction on CC component `c` (or any component when c < 0).
  std::string cond(const u32* w, int c) {
    u32 bits = ((w[1] >> 18) & 1) | (((w[1] >> 19) & 1) << 1) | (((w[1] >> 20) & 1) << 2);
    if (bits == 7) return "true";
    if (bits == 0) return "false";
    std::string ccr = "R.cc[" + std::to_string((w[1] >> 31) & 1) + "].";
    auto test = [&](int comp) {
      std::string v = ccr + kComp[(w[1] >> (21 + 2 * comp)) & 3];
      std::string e;
      auto add = [&](const char* t) { e += (e.empty() ? "" : " || ") + std::string("(") + v + t + ")"; };
      if (bits & 1) add(" < 0");
      if (bits & 2) add(" == 0");
      if (bits & 4) add(" > 0");
      return "(" + e + ")";
    };
    if (c >= 0) return test(c);
    return "(" + test(0) + " || " + test(1) + " || " + test(2) + " || " + test(3) + ")";
  }

  std::string tex_decl(u32 unit) {
    info_->tex_mask |= 1u << unit;
    return "";
  }

  // Texture coordinate vector for `unit` from operand expression `c` (dimension-dependent).
  std::string coords(u32 unit, const std::string& c, bool projective) {
    std::string cc = projective ? "(" + c + " / " + c + ".w)" : c;
    std::string scale = "tex_scale[" + std::to_string(unit) + "]";
    switch (env_.tex_dim[unit]) {
      case 1: return "float2(" + cc + ".x * (" + scale + ".w != 0 ? " + scale + ".x : 1), 0.5)";
      case 3:
      case 4: return cc + ".xyz";
      default: return cc + ".xy * (" + scale + ".w != 0 ? " + scale + ".xy : float2(1, 1))";
    }
  }

  std::string sample(u32 unit, u32 op, const std::string& c, const std::string& s1, const std::string& s2) {
    info_->tex_mask |= 1u << unit;
    if (env_.tex_dim[unit] == 0) return "float4(0, 0, 0, 0)";  // no texture bound
    std::string t = "tex" + std::to_string(unit), s = "smp" + std::to_string(unit);
    bool proj = op == TXP;
    std::string cc = coords(unit, c, proj);
    if (env_.tex_dim[unit] == 5) {  // depth compare: the reference is the next coordinate (z, or z/w)
      std::string ref = proj ? "((" + c + ").z / (" + c + ").w)" : "(" + c + ").z";
      return "(" + t + ".SampleCmpLevelZero(" + s + ", " + cc + ", " + ref + ").xxxx)";
    }
    switch (op) {
      case TXD: {
        u32 dim = env_.tex_dim[unit];
        std::string sel = dim == 3 || dim == 4 ? ".xyz" : dim == 1 ? ".xx" : ".xy";
        return t + ".SampleGrad(" + s + ", " + cc + ", (" + s1 + ")" + sel + ", (" + s2 + ")" + sel + ")";
      }
      case TXL: return t + ".SampleLevel(" + s + ", " + cc + ", (" + s1 + ").x)";
      case TXB: return t + ".SampleBias(" + s + ", " + cc + ", (" + s1 + ").x)";
      default: return t + ".Sample(" + s + ", " + cc + ")";
    }
  }

  // Arithmetic / texture instruction (no flow control). Returns the statement text.
  std::string insn(u32 si) {
    const u32* w = slots_[si].w;
    u32 op = opcode(w);
    if (op == NOP || op == FENCT || op == FENCB) return "";
    if (op == KIL) {
      info_->uses_kil = true;
      return "if (" + cond(w, -1) + ") discard;";
    }
    auto S = [&](int n) { return src(si, n); };
    std::string e;
    bool no_prec = false;
    switch (op) {
      case MOV: e = S(0); break;
      case MUL: e = S(0) + " * " + S(1); break;
      case ADD: e = S(0) + " + " + S(1); break;
      case MAD: e = S(0) + " * " + S(1) + " + " + S(2); break;
      case DP3: e = "dot((" + S(0) + ").xyz, (" + S(1) + ").xyz).xxxx"; break;
      case DP4: e = "dot(" + S(0) + ", " + S(1) + ").xxxx"; break;
      case DP2: e = "dot((" + S(0) + ").xy, (" + S(1) + ").xy).xxxx"; break;
      case DP2A: e = "(dot((" + S(0) + ").xy, (" + S(1) + ").xy) + (" + S(2) + ").x).xxxx"; break;
      case DST: e = "fp_dst(" + S(0) + ", " + S(1) + ")"; break;
      case MIN: e = "min(" + S(0) + ", " + S(1) + ")"; no_prec = true; break;
      case MAX: e = "max(" + S(0) + ", " + S(1) + ")"; no_prec = true; break;
      case SLT: e = "float4(" + S(0) + " < " + S(1) + ")"; break;
      case SGE: e = "float4(" + S(0) + " >= " + S(1) + ")"; break;
      case SLE: e = "float4(" + S(0) + " <= " + S(1) + ")"; break;
      case SGT: e = "float4(" + S(0) + " > " + S(1) + ")"; break;
      case SNE: e = "float4(" + S(0) + " != " + S(1) + ")"; break;
      case SEQ: e = "float4(" + S(0) + " == " + S(1) + ")"; break;
      case FRC: e = "frac(" + S(0) + ")"; no_prec = true; break;
      case FLR: e = "floor(" + S(0) + ")"; break;
      case PK4: e = "fp_pk4(" + S(0) + ")"; break;
      case UP4: e = "fp_up4(" + S(0) + ")"; break;
      case DDX: e = "ddx(" + S(0) + ")"; break;
      case DDY: e = "ddy(" + S(0) + ")"; break;
      case TEX:
      case TXP:
      case TXD:
      case TXL:
      case TXB: {
        u32 unit = (w[0] >> 17) & 0xf;
        bool d = op == TXD, l = op == TXL || op == TXB;
        e = sample(unit, op, S(0), d || l ? S(1) : "", d ? S(2) : "");
        if ((w[0] >> 21) & 1) e = "(" + e + " * 2 - 1)";  // _bx2 expansion
        break;
      }
      case RCP: e = "(1.0 / (" + S(0) + ").x).xxxx"; break;
      case RSQ: e = "rsqrt(abs((" + S(0) + ").x)).xxxx"; break;
      case EX2: e = "exp2((" + S(0) + ").x).xxxx"; break;
      case LG2: e = "log2((" + S(0) + ").x).xxxx"; no_prec = true; break;
      case LIT: e = "fp_lit(" + S(0) + ")"; no_prec = true; break;
      case LIF: e = "fp_lif(" + S(0) + ")"; no_prec = true; break;
      case LRP: e = "lerp(" + S(2) + ", " + S(1) + ", " + S(0) + ")"; break;
      case STR: e = "float4(1, 1, 1, 1)"; break;
      case SFL: e = "float4(0, 0, 0, 0)"; break;
      case COS: e = "cos((" + S(0) + ").x).xxxx"; no_prec = true; break;
      case SIN: e = "sin((" + S(0) + ").x).xxxx"; no_prec = true; break;
      case PK2: e = "fp_pk2(" + S(0) + ")"; break;
      case UP2: e = "fp_up2(" + S(0) + ")"; break;
      case PKB: e = "fp_pkb(" + S(0) + ")"; break;
      case UPB: e = "fp_upb(" + S(0) + ")"; break;
      case PK16: e = "fp_pk16(" + S(0) + ")"; break;
      case UP16: e = "fp_up16(" + S(0) + ")"; break;
      case PKG: e = "fp_pkb(fp_to_srgb(" + S(0) + "))"; break;
      case UPG: e = "fp_to_linear(fp_upb(" + S(0) + "))"; break;
      case BEM: e = "fp_bem(" + S(0) + ", " + S(1) + ", " + S(2) + ")"; break;
      case REFL: e = "fp_refl(" + S(0) + ", " + S(1) + ")"; no_prec = true; break;
      case NRM: e = "normalize((" + S(0) + ").xyz).xyzz"; no_prec = true; break;
      case DIV: e = "(" + S(0) + " / (" + S(1) + ").x)"; break;
      case DIVSQ: e = "fp_divsq(" + S(0) + ", " + S(1) + ")"; break;
      default:
        fail("unsupported fragment opcode " + std::to_string(op));
        return "";
    }
    // Destination pipeline: scale, saturate or precision clamp, mask and condition, CC, register.
    static const char* kScale[8] = {"", " * 2", " * 4", " * 8", "", " * 0.5", " * 0.25", " * 0.125"};
    u32 scale = (w[2] >> 28) & 7;
    std::string out = "{ float4 res = " + e + "; ";
    if (scale && scale != 4) out += std::string("res = res") + kScale[scale] + "; ";
    if (w[0] >> 31) out += "res = saturate(res); ";
    else if (!no_prec) {
      u32 p = (w[0] >> 22) & 3;
      if (p) out += "res = " + prec_clamp("res", p) + "; ";
    }
    u32 mask = (w[0] >> 9) & 0xf;  // bit0 x .. bit3 w
    bool set_cond = (w[0] >> 8) & 1, no_dest = (w[0] >> 30) & 1;
    u32 reg = (w[0] >> 1) & 0x3f;
    std::string dst;
    if ((w[0] >> 7) & 1) {
      max_h_ = std::max(max_h_, reg);
      dst = "R.h[" + std::to_string(reg) + "]";
    } else {
      max_r_ = std::max(max_r_, reg);
      dst = "R.r[" + std::to_string(reg) + "]";
    }
    std::string ccw = "R.cc[" + std::to_string((w[1] >> 30) & 1) + "]";
    bool always = cond(w, 0) == "true";
    for (int c = 0; c < 4; ++c) {
      if (!(mask & (1u << c))) continue;
      std::string guard = always ? "" : "if (" + cond(w, c) + ") ";
      std::string comp(1, kComp[c]);
      if (!no_dest) out += guard + dst + "." + comp + " = res." + comp + "; ";
      if (set_cond) out += guard + ccw + "." + comp + " = res." + comp + "; ";
    }
    return out + "}";
  }

  // Emits slots [begin, end) with structured flow control.
  void range(u32 begin, u32 end, int depth, std::string& out) {
    std::string ind(2 * depth + 2, ' ');
    for (u32 si = begin; si < end && si < slots_.size(); ++si) {
      if (slots_[si].is_const) continue;
      const u32* w = slots_[si].w;
      u32 op = opcode(w);
      if (op == IFE) {
        u32 else_si = (w[2] & 0x7fffffff) >> 2, end_si = w[3] >> 2;
        if (else_si < si || end_si < else_si) {
          fail("malformed IFE");
          continue;
        }
        out += ind + "if (" + cond(w, -1) + ") {\n";
        range(si + 1, else_si, depth + 1, out);
        if (end_si > else_si) {
          out += ind + "} else {\n";
          range(else_si, end_si, depth + 1, out);
        }
        out += ind + "}\n";
        si = end_si - 1;
      } else if (op == LOOP || op == REP) {
        u32 end_si = w[3] >> 2;
        if (end_si <= si) {
          fail("malformed LOOP/REP");
          continue;
        }
        u32 count = (w[2] >> 2) & 0xff, init = (w[2] >> 10) & 0xff;
        s32 step = s32(s8((w[2] >> 19) & 0xff));
        out += ind + "if (" + cond(w, -1) + ") {\n";
        out += ind + "  [loop] for (int it" + std::to_string(depth) + " = 0; it" + std::to_string(depth) + " < " +
               std::to_string(count) + "; ++it" + std::to_string(depth) + ") {\n";
        out += ind + "    lc = " + std::to_string(init) + " + it" + std::to_string(depth) + " * " + std::to_string(step) +
               ";\n";
        range(si + 1, end_si, depth + 2, out);
        out += ind + "  }\n" + ind + "}\n";
        si = end_si - 1;
      } else if (op == BRK) {
        out += ind + "if (" + cond(w, -1) + ") break;\n";
      } else if (op == RET) {
        out += ind + "if (" + cond(w, -1) + ") return;\n";
      } else if (op == CAL) {
        fail("CAL is not implemented");
      } else {
        std::string s = insn(si);
        if (!s.empty()) out += ind + s + "\n";
      }
    }
  }

  u32 max_r() const { return max_r_; }
  u32 max_h() const { return max_h_; }

 private:
  const std::vector<Slot>& slots_;
  const FpEnv& env_;
  FpInfo* info_;
  std::map<u32, u32> const_index_;  // const slot index -> fc[] index
  u32 max_r_ = 4, max_h_ = 8;
};

const char* kHelpers = R"(
float4 fp_dst(float4 a, float4 b) { return float4(1, a.y * b.y, a.z, b.w); }
float4 fp_lit(float4 s) { return float4(1, max(s.x, 0), s.x > 0 ? pow(max(s.y, 0), s.w) : 0, 1); }
float4 fp_lif(float4 s) { return float4(1, s.y, s.y > 0 ? exp2(s.w) : 0, 1); }
float4 fp_pk4(float4 s) { int4 b = int4(round(clamp(s, -1, 1) * 127)) & 0xff; return asfloat(uint(b.x | (b.y << 8) | (b.z << 16) | (b.w << 24))).xxxx; }
float4 fp_up4(float4 s) { uint u = asuint(s.x); int4 b = asint(uint4(u << 24, u << 16, u << 8, u)) >> 24; return max(float4(b) / 127.0, -1); }
float4 fp_pkb(float4 s) { uint4 b = uint4(round(saturate(s) * 255)); return asfloat(b.x | (b.y << 8) | (b.z << 16) | (b.w << 24)).xxxx; }
float4 fp_upb(float4 s) { uint u = asuint(s.x); return float4(u & 0xff, (u >> 8) & 0xff, (u >> 16) & 0xff, u >> 24) / 255.0; }
float4 fp_pk2(float4 s) { return asfloat(f32tof16(s.x) | (f32tof16(s.y) << 16)).xxxx; }
float4 fp_up2(float4 s) { uint u = asuint(s.x); float2 v = float2(f16tof32(u & 0xffff), f16tof32(u >> 16)); return v.xyxy; }
float4 fp_pk16(float4 s) { uint2 b = uint2(round(saturate(s.xy) * 65535)); return asfloat(b.x | (b.y << 16)).xxxx; }
float4 fp_up16(float4 s) { uint u = asuint(s.x); float2 v = float2(u & 0xffff, u >> 16) / 65535.0; return v.xyxy; }
float4 fp_to_srgb(float4 c) { float3 x = saturate(c.rgb); return float4(x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - 0.055, c.a); }
float4 fp_to_linear(float4 c) { float3 x = c.rgb; return float4(x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4), c.a); }
float4 fp_bem(float4 a, float4 b, float4 c) { return a.xyxy + b.xxxx * c.xzxz + b.yyyy * c.ywyw; }
float4 fp_refl(float4 i, float4 n) { return i - 2 * dot(n, i) * n; }
float4 fp_divsq(float4 a, float4 b) { float4 r = a / sqrt(b.x); return a == 0 ? 0 : r; }
bool fp_cmp(uint f, float a, float b) {
  switch (f) { case 1: return a < b; case 2: return a == b; case 3: return a <= b; case 4: return a > b;
               case 5: return a != b; case 6: return a >= b; case 7: return true; default: return false; }
}
float fp_fog_factor(float c) {
  uint mode = asuint(fp_fog.x); float p0 = fp_fog.y, p1 = fp_fog.z;
  if (mode >= 0x802 && mode <= 0x804) c = abs(c);
  float f;
  if (mode == 0x2601 || mode == 0x804) f = p1 * c + p0 - 1;
  else if (mode == 0x801 || mode == 0x803) { float t = 4.709 * (p1 * c + p0 - 1.5); f = exp(-t * t); }
  else f = exp(11.084 * (p1 * c + p0 - 1.5));
  return saturate(f);
}
)";

}  // namespace

namespace {
// fp_scan results per program address (called for every draw): a program whose instruction slots
// are unchanged has the same layout, hash and constant positions; its inline constants (patched
// by games between draws) are not compared, the caller reads them afresh.
struct ScanMemo {
  const u8* ucode = nullptr;
  u32 size = 0, max_bytes = 0;
  u64 hash = 0;
  std::vector<u32> const_offsets;  // relative to the first real instruction (fp_scan's contract)
  std::vector<u32> const_slots;    // byte offsets from ucode of the constant slots (ascending)
  std::vector<u8> code;            // the program's bytes when scanned
};

bool memo_valid(const ScanMemo& m, const u8* ucode, u32 max_bytes) {
  if (m.ucode != ucode || m.max_bytes != max_bytes || !m.size) return false;
  size_t k = 0;
  for (u32 off = 0; off < m.size; off += 16) {
    if (k < m.const_slots.size() && m.const_slots[k] == off) {
      ++k;
      continue;
    }
    if (std::memcmp(ucode + off, m.code.data() + off, 16)) return false;
  }
  return true;
}
}  // namespace

u32 fp_scan(const u8* ucode, u32 max_bytes, u64* code_hash, std::vector<u32>* const_offsets) {
  static thread_local ScanMemo memo[256];
  ScanMemo& m = memo[(reinterpret_cast<uintptr_t>(ucode) >> 4) & 255];
  if (memo_valid(m, ucode, max_bytes)) {
    if (code_hash) *code_hash = m.hash;
    if (const_offsets) const_offsets->insert(const_offsets->end(), m.const_offsets.begin(), m.const_offsets.end());
    return m.size;
  }
  static thread_local std::vector<Slot> slots;  // reused: called for every draw
  slots.clear();
  u32 size = 0;
  if (!scan(ucode, max_bytes, &slots, &size)) return 0;
  u64 h = 0xcbf29ce484222325ull;
  m.const_offsets.clear();
  m.const_slots.clear();
  u32 base = size - (slots.empty() ? 0 : slots.back().offset + 16);  // leading NOPs skipped by scan
  for (const Slot& s : slots) {
    if (s.is_const) {
      m.const_offsets.push_back(s.offset);
      m.const_slots.push_back(base + s.offset);
      continue;
    }
    for (u32 v : s.w) {
      h ^= v;
      h *= 0x100000001b3ull;
    }
  }
  h ^= slots.size();
  m.ucode = ucode;
  m.size = size;
  m.max_bytes = max_bytes;
  m.hash = h;
  m.code.assign(ucode, ucode + size);
  if (code_hash) *code_hash = h;
  if (const_offsets) const_offsets->insert(const_offsets->end(), m.const_offsets.begin(), m.const_offsets.end());
  return size;
}

std::string fp_to_hlsl(const u8* ucode, u32 max_bytes, const FpEnv& env, FpInfo* info) {
  *info = FpInfo{};
  std::vector<Slot> slots;
  u32 size = 0;
  if (!scan(ucode, max_bytes, &slots, &size)) {
    info->error = "fragment program has no END within the readable range";
    return "";
  }
  info->size = size;
  Emitter em(slots, env, info);
  std::string body;
  em.range(0, u32(slots.size()), 0, body);

  std::string s;
  s += "cbuffer FpState : register(b2) { float4 fp_wpos; uint4 fp_flags; float4 fp_alpha; float4 fp_fog; float4 tex_scale[16]; };\n";
  s += "cbuffer FpConst : register(b3) { float4 fc[256]; };\n";
  for (u32 u = 0; u < 16; ++u) {
    if (!((info->tex_mask >> u) & 1) || env.tex_dim[u] == 0) continue;
    const char* type = env.tex_dim[u] == 3 ? "Texture3D" : env.tex_dim[u] == 4 ? "TextureCube" : "Texture2D";
    const char* sampler = env.tex_dim[u] == 5 ? "SamplerComparisonState" : "SamplerState";
    s += std::string(type) + " tex" + std::to_string(u) + " : register(t" + std::to_string(u) + "); " + sampler + " smp" +
         std::to_string(u) + " : register(s" + std::to_string(u) + ");\n";
  }
  s += "struct PsIn { float4 pos : SV_Position;";
  for (int i = 0; i < 16; ++i) s += " float4 o" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";";
  s += " };\n";
  s += "struct Regs { float4 r[" + std::to_string(em.max_r() + 1) + "]; float4 h[" + std::to_string(em.max_h() + 1) +
       "]; float4 cc[2]; };\n";
  s += "static float4 wpos, col0, col1, fogc, ssa, hpos; static float4 tc[10]; static float4 inv_w; static int lc;\n";
  s += kHelpers;
  s += "void prog(inout Regs R) {\n" + body + "}\n";
  s += "struct PsOut { float4 c0 : SV_Target0;";
  for (u32 i = 1; i < env.mrt_count; ++i) s += " float4 c" + std::to_string(i) + " : SV_Target" + std::to_string(i) + ";";
  if (env.depth_export) s += " float depth : SV_Depth;";
  s += " };\n";
  s += "PsOut main(PsIn i, bool front : SV_IsFrontFace) {\n";
  s += "  hpos = i.o0; inv_w = (1.0 / hpos.w).xxxx; lc = 0;\n";
  // fp_alpha.zw: 1 / samples per pixel in x and y (antialiased surfaces are rendered at storage size).
  s += "  float2 sp = i.pos.xy * fp_alpha.zw;\n";
  s += "  float wy = fp_wpos.y != 0 ? fp_wpos.x - sp.y : sp.y;\n";
  s += "  float off = fp_wpos.z != 0 ? 0.5 : 0;\n";
  s += "  wpos = float4(sp.x - off, wy - off, i.pos.z, inv_w.x);\n";
  s += "  bool back = fp_wpos.w != 0 && !front;\n";
  s += "  col0 = saturate(back ? i.o3 : i.o1); col1 = saturate(back ? i.o4 : i.o2);\n";
  s += "  fogc = float4(i.o5.x, fp_fog_factor(i.o5.x), 0, 0);\n";
  s += "  tc[0] = i.o7; tc[1] = i.o8; tc[2] = i.o9; tc[3] = i.o10; tc[4] = i.o11; tc[5] = i.o12; tc[6] = i.o13; tc[7] = i.o14;\n";
  s += "  tc[8] = i.o15; tc[9] = i.o6;\n";
  s += "  [unroll] for (int k = 0; k < 10; ++k) if ((fp_flags.x >> k) & 1) tc[k] = float4(tc[k].xy, 0, hpos.w);\n";
  s += "  ssa = front ? float4(1, 1, 1, 1) : float4(-1, -1, -1, -1);\n";
  s += "  Regs R = (Regs)0;\n  prog(R);\n  PsOut o;\n";
  static const int kOut32[4] = {0, 2, 3, 4}, kOut16[4] = {0, 4, 6, 8};
  for (u32 c = 0; c < env.mrt_count; ++c)
    s += "  o.c" + std::to_string(c) + " = " +
         (env.fp32_exports ? "R.r[" + std::to_string(kOut32[c]) + "]" : "R.h[" + std::to_string(kOut16[c]) + "]") + ";\n";
  s += "  if (fp_flags.y != 0 && !fp_cmp(fp_flags.y - 1, o.c0.a, fp_alpha.x)) discard;\n";
  if (env.depth_export) s += "  o.depth = R.r[1].z;\n";
  s += "  return o;\n}\n";
  return s;
}

}  // namespace rt::gpu
