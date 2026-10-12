// ac4-recomp: PPU static recompiler.
//   ac4-recomp analyze <eboot.elf>                 print analysis statistics
//   ac4-recomp disasm  <eboot.elf> <addr> [count]  disassemble
//   ac4-recomp func    <eboot.elf> <addr>          show one analysed function
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
            opt.hooks[u32(std::stoul(addr, nullptr, 0))] = name;
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
