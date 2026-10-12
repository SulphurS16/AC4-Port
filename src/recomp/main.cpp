// ac4-recomp: PPU static recompiler.
//   ac4-recomp analyze <eboot.elf>                 print analysis statistics
//   ac4-recomp disasm  <eboot.elf> <addr> [count]  disassemble
//   ac4-recomp func    <eboot.elf> <addr>          show one analysed function
//   ac4-recomp fingerprint <eboot.elf>             per-function hashes and calls (cross-build matching)
//   ac4-recomp emit    <eboot.elf> <out-dir>       generate C++ sources
#include <chrono>
#include <filesystem>
#include <memory>
#include <cstdio>
#include <sstream>
#include <stdexcept>

#include "common/file.h"
#include "recomp/analysis.h"
#include "recomp/emit.h"

using namespace recomp;

static u32 parse_u32(const char* s) { return u32(std::strtoul(s, nullptr, 0)); }

// Per-function fingerprints for matching functions between two builds of related code (e.g. the
// same engine in another game): one line per function
//   func <entry> <insns> <exact hash> <shape hash> <calls...>
// "exact" masks only what differs with code placement (branch displacements, TOC offsets, lis/
// low-half address pairs); "shape" masks every 16-bit immediate. Calls are listed in address order
// as function entries or "imp:<module>:<fnid>" for import stubs.
static int cmd_fingerprint(const Image& img, const Analysis& an) {
  auto fnv = [](u64 h, u32 w) {
    for (int i = 0; i < 4; ++i) h = (h ^ ((w >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    return h;
  };
  auto dform = [](u32 op) {
    return op == 7 || op == 8 || (op >= 10 && op <= 15) || (op >= 24 && op <= 29) || (op >= 32 && op <= 55) || op == 58 || op == 62;
  };
  std::map<u32, std::pair<std::string, u32>> slot_import;
  for (const auto& im : img.imports()) slot_import[im.slot] = {im.module, im.fnid};
  auto stub_import = [&](u32 s, std::string* out) {
    if (!img.mapped(s, 32) || img.r32(s) != 0x39800000 || (img.r32(s + 4) >> 16) != 0x658c) return false;
    u32 slot = ((img.r32(s + 4) & 0xffff) << 16) + u32(s32(s16(img.r32(s + 8) & 0xffff)));
    auto it = slot_import.find(slot);
    if (it == slot_import.end()) return false;
    *out = ac4::strprintf("imp:%s:%08x", it->second.first.c_str(), it->second.second);
    return true;
  };
  for (const auto& [entry, f] : an.functions()) {
    if (f.secondary) continue;
    u64 he = 0xcbf29ce484222325ull, hs = he;
    int lis_reg = -1, lis_left = 0;
    std::string calls;
    for (u32 a : f.insns) {
      u32 w = img.r32(a), op = w >> 26, rt = (w >> 21) & 31, ra = (w >> 16) & 31;
      u32 e = w, s = w;
      if (op == 18) {  // b / bl
        e = s = w & 0xFC000003u;
        if (w & 1) {
          u32 t = a + u32(s32(w << 6) >> 6) - (w & 2 ? a : 0);
          t &= ~3u;
          std::string imp;
          calls += stub_import(t, &imp) ? " " + imp : ac4::strprintf(" %08x", t);
        }
      } else if (op == 16) {  // bc
        e = s = w & 0xFFFF0003u;
      } else if (dform(op)) {
        s = w & 0xFFFF0000u;
        bool addr_imm = ra == 2 || (op == 15 && ra == 0) || (lis_left > 0 && int(ra) == lis_reg && op != 15);
        if (addr_imm) e = w & 0xFFFF0000u;
      }
      if (op == 15 && ra == 0) {  // lis rT: the next few instructions may complete an address
        lis_reg = int(rt);
        lis_left = 6;
      } else if (lis_left > 0) {
        --lis_left;
      }
      he = fnv(he, e);
      hs = fnv(hs, s);
    }
    std::printf("func %08x %zu %016llx %016llx%s\n", entry, f.insns.size(), (unsigned long long)he, (unsigned long long)hs,
                calls.c_str());
  }
  return 0;
}

int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      std::fprintf(stderr, "usage: ac4-recomp analyze|disasm|func|emit <eboot.elf> ...\n");
      return 2;
    }
    std::string cmd = argv[1];
    Image img(ac4::read_file(argv[2]));
    if (cmd == "disasm" && argc >= 4) {
      u32 a = parse_u32(argv[3]), n = argc >= 5 ? parse_u32(argv[4]) : 32;
      for (u32 i = 0; i < n; ++i, a += 4) {
        if (!img.mapped(a, 4)) break;
        char buf[128];
        ppu::disasm(a, img.r32(a), buf, sizeof buf);
        std::printf("%08x: %08x  %s\n", a, img.r32(a), buf);
      }
      return 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    Analysis an(img);
    an.run();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto& st = an.stats();
    if (cmd == "analyze") {
      std::printf("analysis: %.2fs, %zu passes\n", secs, st.passes);
      std::printf("opd entries %zu, functions %zu (secondary %zu), reachable insns %zu\n", st.opd_entries, st.functions,
                  st.secondary, st.insns);
      std::printf("switch tables %zu (%zu targets), unresolved bctr %zu, invalid reachable words %zu\n", st.switches,
                  st.switch_targets, st.unresolved_bctr, st.invalid);
      size_t shown = 0;
      for (u32 a : st.unresolved_bctr_sites)
        if (shown++ < 40) std::printf("  unresolved bctr at 0x%08x\n", a);
      shown = 0;
      for (u32 a : st.invalid_sites)
        if (shown++ < 40) std::printf("  invalid word at 0x%08x: %08x\n", a, img.r32(a));
      // Code coverage of executable sections.
      size_t total = 0;
      for (const auto& [b, e] : img.code_ranges()) total += (e - b) / 4;
      std::printf("code words %zu, covered %.2f%%\n", total, 100.0 * double(st.insns) / double(total));
      return 0;
    }
    if (cmd == "fingerprint") return cmd_fingerprint(img, an);
    if (cmd == "func" && argc >= 4) {
      u32 a = parse_u32(argv[3]);
      auto it = an.functions().upper_bound(a);
      if (it == an.functions().begin()) throw std::runtime_error("no function");
      --it;
      const Function& f = it->second;
      std::printf("function 0x%08x range [0x%08x,0x%08x) insns %zu%s\n", f.entry, f.begin, f.end, f.insns.size(),
                  f.secondary ? " (secondary)" : "");
      for (u32 x : f.insns) {
        char buf[128];
        ppu::disasm(x, img.r32(x), buf, sizeof buf);
        std::printf("%s%08x: %s\n", f.labels.count(x) ? "L " : "  ", x, buf);
      }
      for (u32 t : f.external) std::printf("  external -> 0x%08x\n", t);
      return 0;
    }
    if (cmd == "emit" && argc >= 4) {
      EmitOptions opt;
      std::vector<std::unique_ptr<Image>> mods;
      std::vector<std::unique_ptr<Analysis>> mans;
      std::vector<Unit> units{{&img, &an, "ppu"}};
      for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--comments") opt.comments = true;
        else if (a == "--hooks" && i + 1 < argc) {  // lines "0xADDR name [# comment]"
          std::vector<u8> raw = ac4::read_file(argv[++i]);
          std::istringstream in(std::string(raw.begin(), raw.end()));
          std::string line;
          while (std::getline(in, line)) {
            if (size_t c = line.find('#'); c != std::string::npos) line.resize(c);
            std::istringstream ls(line);
            std::string addr, name;
            if (!(ls >> addr >> name)) continue;
            if (name == "setjmp") opt.setjmp_addr = u32(std::stoul(addr, nullptr, 0));  // call sites, not a hook
            else opt.hooks[u32(std::stoul(addr, nullptr, 0))] = name;
          }
          std::printf("%zu hook(s)\n", opt.hooks.size());
        }
        else if (a.rfind("--chunk=", 0) == 0) opt.insns_per_file = std::strtoul(a.c_str() + 8, nullptr, 0);
        else if (a == "--module" && i + 1 < argc) {
          std::filesystem::path mp(argv[++i]);
          mods.push_back(std::make_unique<Image>(ac4::read_file(mp)));
          mans.push_back(std::make_unique<Analysis>(*mods.back()));
          mans.back()->run();
          units.push_back({mods.back().get(), mans.back().get(), mp.stem().string()});
          std::printf("module %s ('%s'): %zu functions, %zu exports, %zu imports\n", mp.stem().string().c_str(),
                      mods.back()->module_name().c_str(), mans.back()->functions().size(), mods.back()->exports().size(),
                      mods.back()->imports().size());
        }
      }
      emit_all(units, argv[3], opt);
      return 0;
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
