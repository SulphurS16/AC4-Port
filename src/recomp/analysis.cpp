#include "recomp/analysis.h"

#include <algorithm>
#include <cstdio>

namespace recomp {

using namespace ppu;

bool Analysis::is_stub(u32 a) const {
  // Import stubs: li r12,0; oris r12,r12,hi; lwz r12,lo(r12); ... bctr (8 words; a = the bctr)
  u32 s = a - 28;
  return img_.mapped(s, 32) && img_.r32(s) == 0x39800000 && (img_.r32(s + 4) >> 16) == 0x658c && img_.r32(s + 28) == 0x4e800420;
}

u32 Analysis::code_end_for(u32 a) const {
  for (const auto& [b, e] : img_.code_ranges())
    if (a >= b && a < e) return e;
  return a;
}

bool Analysis::detect_switch(Function& f, u32 pc, std::vector<u32>& targets) {
  // Walk back from the bctr through straight-line code.
  int ctr_reg = -1;
  bool relative = false, saw_load = false;
  u32 count = 0, mask_bound = 0;
  int base_reg = -1;
  u32 base_toc_disp = 0;
  bool base_from_toc = false;
  for (u32 k = 1; k <= 24; ++k) {
    u32 a = pc - 4 * k;
    if (a < f.begin || !img_.in_code(a)) break;
    u32 w = img_.r32(a);
    Op op = decode(w);
    if (op == Op::MTSPR && spr(w) == 9 && ctr_reg < 0) {
      ctr_reg = int(rd(w));
      continue;
    }
    if (ctr_reg >= 0 && op == Op::ADD && int(rd(w)) == ctr_reg && !relative) {
      relative = true;
      continue;
    }
    if (op == Op::LWZX || op == Op::LWAX) {
      saw_load = true;
      if (base_reg < 0) base_reg = int(rb(w));  // lwzx rY, rI, rT (index first) is the common form
      continue;
    }
    if (op == Op::LWZ && int(ra(w)) == 2 && base_reg >= 0 && int(rd(w)) == base_reg) {
      base_from_toc = true;
      base_toc_disp = u32(simm(w));
      continue;
    }
    if (op == Op::RLWINM && rb(w) == 2 && me(w) == 29 && !mask_bound) {
      // rlwinm rX,rY,2,mb,29 = (rY << 2) & bits mb..29: at most 2^(30-mb) table entries.
      u32 mb = rc_(w);
      if (mb >= 2 && mb <= 29) mask_bound = 1u << (30 - mb);
      continue;
    }
    if (op == Op::CMPLI && cmp_l(w) == 0 && count == 0) {
      count = uimm(w) + 1;
      break;
    }
    // Conditional branches/returns are guards inside the block; anything else ends it.
    if (op == Op::BC && !lk(w)) continue;
    if (op == Op::BCLR && !lk(w) && (rd(w) & 0x14) != 0x14) continue;
    if (is_branch(op)) break;
  }
  if (ctr_reg < 0 || !saw_load) return false;
  if (count > 4096) return false;
  if (count == 0 && !relative) return false;  // absolute tables need an explicit bound
  u32 table = pc + 4;
  if (count == 0) {
    // Self-delimiting relative table: take entries while they are plausible targets.
    u32 limit = mask_bound ? std::min<u32>(mask_bound, 1024) : 1024;
    u32 n = 0;
    while (n < limit && img_.mapped(table + 4 * n, 4)) {
      u32 t = table + img_.r32(table + 4 * n);
      if ((t & 3) || t < f.begin || t >= f.end || !img_.in_code(t) || (t >= table && t < table + 4 * (n + 1))) break;
      if (decode(img_.r32(t)) == Op::INVALID) break;
      ++n;
    }
    if (n < 2) return false;
    count = n;
  }
  // Cross-check the TOC-loaded base when this function's TOC is known.
  if (base_from_toc) {
    auto it = toc_.find(f.entry);
    if (it != toc_.end()) {
      u32 ea = it->second + base_toc_disp;
      if (img_.mapped(ea, 4) && img_.r32(ea) != table) return false;
    }
  }
  targets.clear();
  for (u32 i = 0; i < count; ++i) {
    if (!img_.mapped(table + 4 * i, 4)) return false;
    u32 v = img_.r32(table + 4 * i);
    u32 t = relative ? table + v : v;
    if ((t & 3) || t < f.begin || t >= f.end || !img_.in_code(t)) return false;
    if (t >= table && t < table + 4 * count) return false;
    if (decode(img_.r32(t)) == Op::INVALID) return false;
    targets.push_back(t);
  }
  return true;
}

// Absolute jump table whose address was loaded from the TOC and spilled to the stack:
//   lwz rS,d(r2) .. std rS,off(r1) .. ld rB,off(r1); rlwinm rI,rX,2,mb,29; lwzx rT,rB,rI; mtctr rT; bctr
// The table has no explicit bound: entries are taken while they are plausible targets inside
// the function (code, aligned, decodable).
bool Analysis::detect_spilled_table(Function& f, u32 pc, std::vector<u32>& targets) {
  int ctr = -1, idx = -1, la = -1, lb = -1;
  u32 bound = 0;
  std::map<int, s32> stack_loads;  // register -> stack slot it was reloaded from (closest to the bctr)
  for (u32 k = 1; k <= 24; ++k) {
    u32 a = pc - 4 * k;
    if (a < f.begin || !img_.in_code(a)) break;
    u32 w = img_.r32(a);
    Op op = decode(w);
    if (op == Op::MTSPR && spr(w) == 9 && ctr < 0) {
      ctr = int(rd(w));
    } else if (op == Op::LWZX && la < 0 && ctr >= 0 && int(rd(w)) == ctr) {
      la = int(ra(w));
      lb = int(rb(w));
    } else if (op == Op::RLWINM && rb(w) == 2 && me(w) == 29 && idx < 0 && la >= 0) {
      idx = int(ra(w));
      u32 mb = rc_(w);
      if (mb >= 2 && mb <= 29) bound = 1u << (30 - mb);
    } else if ((op == Op::LD || op == Op::LWZ) && ra(w) == 1 && !stack_loads.count(int(rd(w)))) {
      stack_loads[int(rd(w))] = op == Op::LD ? ds(w) : simm(w);
    } else if (op == Op::B || op == Op::BCCTR || (op == Op::BCLR && (rd(w) & 0x14) == 0x14)) {
      break;
    }
  }
  if (ctr < 0 || la < 0 || idx < 0) return false;
  int base = lb == idx ? la : la == idx ? lb : -1;
  if (base < 0 || !stack_loads.count(base)) return false;
  s32 off = stack_loads[base];
  int src = -1;
  u32 store_at = 0;
  for (u32 a = pc - 4; a >= f.begin && a < pc; a -= 4) {
    u32 w = img_.r32(a);
    Op op = decode(w);
    if (ra(w) == 1 && ((op == Op::STD && ds(w) == off) || (op == Op::STW && simm(w) == off))) {
      src = int(rd(w));
      store_at = a;
      break;
    }
  }
  if (src < 0) return false;
  s32 disp = 0;
  bool wide = false, found = false;
  for (u32 a = store_at - 4, n = 0; a >= f.begin && a < store_at && n < 256; a -= 4, ++n) {
    u32 w = img_.r32(a);
    Op op = decode(w);
    if ((op == Op::LWZ || op == Op::LD) && int(rd(w)) == src) {
      if (ra(w) != 2) return false;
      disp = op == Op::LD ? ds(w) : simm(w);
      wide = op == Op::LD;
      found = true;
      break;
    }
  }
  auto it = toc_.find(f.entry);
  u32 toc = it != toc_.end() ? it->second : module_toc_;
  if (!found || !toc) return false;
  u32 ea = toc + u32(disp);
  if (!img_.mapped(ea, wide ? 8 : 4)) return false;
  u32 table = img_.r32(wide ? ea + 4 : ea);
  u32 limit = std::min<u32>(bound ? bound : 1024, 1024);
  targets.clear();
  for (u32 n = 0; n < limit && img_.mapped(table + 4 * n, 4); ++n) {
    u32 t = img_.r32(table + 4 * n);
    if ((t & 3) || t < f.begin || t >= f.end || !img_.in_code(t) || decode(img_.r32(t)) == Op::INVALID) break;
    targets.push_back(t);
  }
  return targets.size() >= 2;
}

// Computed goto through a TOC slot holding one label address: lwz rX,d(r2); [clrldi rX,rX,32];
// mtctr rX; bctr.
bool Analysis::detect_toc_jump(Function& f, u32 pc, std::vector<u32>& targets) {
  int reg = -1;
  for (u32 k = 1; k <= 4; ++k) {
    u32 a = pc - 4 * k;
    if (a < f.begin || !img_.in_code(a)) return false;
    u32 w = img_.r32(a);
    Op op = decode(w);
    if (reg < 0) {
      if (op != Op::MTSPR || spr(w) != 9) return false;
      reg = int(rd(w));
    } else if (op == Op::RLDICL && int(ra(w)) == reg && int(rd(w)) == reg) {
      continue;  // clrldi: the address is 32-bit
    } else if (op == Op::LWZ && int(rd(w)) == reg && ra(w) == 2) {
      auto it = toc_.find(f.entry);
      u32 toc = it != toc_.end() ? it->second : module_toc_;
      u32 ea = toc + u32(simm(w));
      if (!toc || !img_.mapped(ea, 4)) return false;
      u32 t = img_.r32(ea);
      if ((t & 3) || t < f.begin || t >= f.end || !img_.in_code(t) || decode(img_.r32(t)) == Op::INVALID) return false;
      targets.assign(1, t);
      return true;
    } else {
      return false;
    }
  }
  return false;
}

void Analysis::traverse(Function& f) {
  std::set<u32> reach;
  std::vector<u32> work{f.entry};
  f.labels.clear();
  f.switches.clear();
  f.calls.clear();
  f.external.clear();
  f.invalid = 0;
  auto in_range = [&](u32 t) { return t >= f.begin && t < f.end && img_.in_code(t); };
  auto jump = [&](u32 t) {
    if (in_range(t)) {
      f.labels.insert(t);
      work.push_back(t);
    } else {
      f.external.insert(t);
    }
  };
  while (!work.empty()) {
    u32 pc = work.back();
    work.pop_back();
    for (;;) {
      if (reach.count(pc)) break;
      if (!in_range(pc)) {
        f.external.insert(pc);  // fallthrough leaves the range: tail call
        break;
      }
      reach.insert(pc);
      u32 w = img_.r32(pc);
      Op op = decode(w);
      bool cont = true;
      switch (op) {
        case Op::INVALID:
          ++f.invalid;
          st_.invalid_sites.push_back(pc);
          cont = false;
          break;
        case Op::B: {
          u32 t = (aa(w) ? 0 : pc) + u32(li(w));
          if (lk(w)) {
            f.calls.insert(t);
          } else {
            jump(t);
            cont = false;
          }
          break;
        }
        case Op::BC: {
          u32 t = (aa(w) ? 0 : pc) + u32(bd(w));
          u32 bo = rd(w);
          bool always = (bo & 0x14) == 0x14;
          if (lk(w)) {
            if (t != pc + 4) f.calls.insert(t);
          } else {
            jump(t);
            if (always) cont = false;
          }
          break;
        }
        case Op::BCLR:
          if (!lk(w) && (rd(w) & 0x14) == 0x14) cont = false;
          break;
        case Op::BCCTR:
          if (!lk(w)) {
            std::vector<u32> tg;
            if (detect_switch(f, pc, tg) || detect_spilled_table(f, pc, tg) || detect_toc_jump(f, pc, tg)) {
              for (u32 t : tg) {
                f.labels.insert(t);
                work.push_back(t);
              }
              f.switches[pc] = tg;
            }
            if ((rd(w) & 0x14) == 0x14) cont = false;
          }
          break;
        case Op::TW:
        case Op::TWI:
        case Op::TD:
        case Op::TDI:
          if (rd(w) == 31) cont = false;  // unconditional trap
          break;
        default:
          break;
      }
      if (!cont) break;
      pc += 4;
    }
  }
  f.insns.assign(reach.begin(), reach.end());
}

void Analysis::run(const std::set<u32>& extra_entries) {
  // Seed: OPD entries (function descriptors), the entry descriptor and explicit extras.
  std::set<u32> entries;
  for (const auto& [fn, toc] : img_.opd_entries()) {
    if (!img_.in_code(fn) || (fn & 3)) continue;
    entries.insert(fn);
    toc_.emplace(fn, toc);
    ++st_.opd_entries;
  }
  module_toc_ = toc_.empty() ? 0 : toc_.begin()->second;
  for (const auto& [fn, toc] : toc_)
    if (toc != module_toc_) module_toc_ = 0;
  if (img_.entry_opd() && img_.mapped(img_.entry_opd(), 8)) entries.insert(img_.r32(img_.entry_opd()));
  for (const auto& [b, e] : img_.code_ranges()) entries.insert(b);  // .init/.fini/.text/stub starts
  for (u32 x : extra_entries) entries.insert(x);

  std::set<u32> secondary;
  for (int pass = 0; pass < 16; ++pass) {
    ++st_.passes;
    funcs_.clear();
    std::vector<u32> sorted(entries.begin(), entries.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
      Function f;
      f.entry = f.begin = sorted[i];
      u32 cend = code_end_for(f.entry);
      f.end = (i + 1 < sorted.size() && sorted[i + 1] < cend) ? sorted[i + 1] : cend;
      traverse(f);
      funcs_.emplace(f.entry, std::move(f));
    }
    // Secondary (jump-in) entries keep the containing function's range end.
    for (u32 s : secondary) {
      if (funcs_.count(s)) continue;
      auto it = funcs_.upper_bound(s);
      if (it == funcs_.begin()) continue;
      --it;
      Function f;
      f.entry = f.begin = s;
      f.end = it->second.end;
      f.secondary = true;
      traverse(f);
      funcs_.emplace(s, std::move(f));
    }
    // Classify newly referenced targets.
    std::set<u32> reached;  // instruction addresses reachable inside some function
    for (const auto& [e, f] : funcs_)
      for (u32 a : f.insns) reached.insert(a);
    size_t before_e = entries.size(), before_s = secondary.size();
    for (const auto& [e, f] : funcs_) {
      auto consider = [&](u32 t) {
        if (entries.count(t) || secondary.count(t) || !img_.in_code(t) || (t & 3)) return;
        if (reached.count(t)) secondary.insert(t);  // inside existing code: don't split
        else entries.insert(t);                      // code in a gap: a new (static) function
      };
      for (u32 t : f.calls) consider(t);
      for (u32 t : f.external) consider(t);
    }
    if (entries.size() == before_e && secondary.size() == before_s) break;
  }
  st_ = AnalysisStats{st_.opd_entries, 0, 0, 0, 0, 0, 0, 0, st_.passes, {}, {}};
  for (const auto& [e, f] : funcs_) {
    ++st_.functions;
    if (f.secondary) ++st_.secondary;
    st_.insns += f.insns.size();
    st_.switches += f.switches.size();
    for (const auto& [a, t] : f.switches) st_.switch_targets += t.size();
    st_.invalid += f.invalid;
    for (u32 a : f.insns) {
      u32 w = img_.r32(a);
      if (decode(w) == Op::BCCTR && !lk(w) && !f.switches.count(a) && !is_stub(a)) {
        ++st_.unresolved_bctr;
        st_.unresolved_bctr_sites.push_back(a);
      }
      if (decode(w) == Op::INVALID) st_.invalid_sites.push_back(a);
    }
  }
}

}  // namespace recomp
