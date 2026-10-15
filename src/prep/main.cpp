// ac4-prep: local preparation of the user's own PS3 game inputs.
//   ac4-prep import --disc <disc> --eboot <EBOOT.elf> --modules-dir <dir> --flash-file <flashATRAC.pic>
//                    --out <prepared-dir> [--flash-out <dev_flash-dir>]
//                                         (the player's decrypted files, verified by SHA-256)
//   ac4-prep cpu                         (exit code 3 if the CPU lacks x86-64-v3 features)
//   ac4-prep spu-records --manifest <spu_manifest.txt> --out <spu_cache-dir> <prepared-dir>
//                                         (SPU code records rebuilt from the user's prepared files)
// Development builds only (key-based path, not in the release):
//   ac4-prep identify <game-root>
//   ac4-prep pkg      <file.pkg> <out-dir>   [--keys <file>]
//   ac4-prep self     <file.self> <out.elf>  [--keys <file>]
//   ac4-prep prepare  --game <game-root> --out <dir> [--keys <file>] [--update <file.pkg>|none]
//                      (update packages: the given ones, none, or by default <game-root>/Updates/*.pkg)
//   ac4-prep firmware <PS3UPDAT.PUP> <out-dir>  [--keys <file>]   (dev_flash extraction)
//   ac4-prep lle --firmware <fw-dir> --out <prepared-dir> [--modules libsre,...]  (relocated LLE modules)
// Inputs are only read; every write is checked against the protected roots.
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "prep/formats.h"

using namespace ac4;
using namespace ac4::prep;

namespace {

std::string arg_value(int& i, int argc, char** argv) {
  if (i + 1 >= argc) throw std::runtime_error(std::string("missing value after ") + argv[i]);
  return argv[++i];
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::fprintf(stderr, "usage: ac4-prep import|cpu|spu-records ... (see source header)\n");
      return 2;
    }
    std::string cmd = argv[1];
    std::string keyfile, game, out, firmware, manifest, flash_out;
    std::vector<std::string> modules = {"libsre"}, updates;
    std::vector<std::string> pos;
    ImportInputs imp;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--keys") keyfile = arg_value(i, argc, argv);
      else if (a == "--game") game = arg_value(i, argc, argv);
      else if (a == "--out") out = arg_value(i, argc, argv);
      else if (a == "--firmware") firmware = arg_value(i, argc, argv);
      else if (a == "--manifest") manifest = arg_value(i, argc, argv);
      else if (a == "--update") updates.push_back(arg_value(i, argc, argv));
      else if (a == "--disc") imp.disc = arg_value(i, argc, argv);
      else if (a == "--eboot") imp.eboot_elf = arg_value(i, argc, argv);
      else if (a == "--modules-dir") imp.modules_dir = arg_value(i, argc, argv);
      else if (a == "--flash-file") imp.flash_atrac = arg_value(i, argc, argv);
      else if (a == "--flash-out") flash_out = arg_value(i, argc, argv);
      else if (a == "--modules") {
        modules.clear();
        std::string v = arg_value(i, argc, argv);
        for (size_t p = 0; p <= v.size();) {
          size_t q = v.find(',', p);
          if (q == std::string::npos) q = v.size();
          if (q > p) modules.push_back(v.substr(p, q - p));
          p = q + 1;
        }
      }
      else pos.push_back(a);
    }
    if (cmd == "import" && !out.empty() && !imp.disc.empty() && !imp.eboot_elf.empty() && !imp.modules_dir.empty() &&
        !imp.flash_atrac.empty()) {
      fs::path fo = flash_out.empty() ? fs::path(out).parent_path() / "dev_flash" : fs::path(flash_out);
      std::fputs(import_inputs(imp, out, fo).c_str(), stdout);
      return 0;
    }
    if (cmd == "cpu") {  // the generated code and runtime are built for x86-64-v3 (CMakeLists AC4_CPU_FLAGS)
      __builtin_cpu_init();
      const char* need[] = {"avx", "avx2", "fma", "bmi", "bmi2", "sse4.2", "popcnt"};
      std::string missing;
      for (const char* f : need) {
        bool has = false;
        if (!std::strcmp(f, "avx")) has = __builtin_cpu_supports("avx");
        else if (!std::strcmp(f, "avx2")) has = __builtin_cpu_supports("avx2");
        else if (!std::strcmp(f, "fma")) has = __builtin_cpu_supports("fma");
        else if (!std::strcmp(f, "bmi")) has = __builtin_cpu_supports("bmi");
        else if (!std::strcmp(f, "bmi2")) has = __builtin_cpu_supports("bmi2");
        else if (!std::strcmp(f, "sse4.2")) has = __builtin_cpu_supports("sse4.2");
        else if (!std::strcmp(f, "popcnt")) has = __builtin_cpu_supports("popcnt");
        if (!has) missing += std::string(missing.empty() ? "" : ", ") + f;
      }
      if (!missing.empty()) {
        std::printf("this CPU lacks %s: the port needs an x86-64-v3 CPU (AVX2, FMA3, BMI2; Intel Haswell 2013 or AMD Excavator / "
                    "Zen or newer)\n", missing.c_str());
        return 3;
      }
      std::printf("CPU supports x86-64-v3 (AVX2, FMA3, BMI1/2)\n");
      return 0;
    }
    if (cmd == "spu-records" && !manifest.empty() && !out.empty() && pos.size() == 1) {
      SpuRecordStats st = rebuild_spu_records(manifest, pos[0], out);
      std::fputs(st.notes.c_str(), stdout);
      std::printf("SPU code records from %s: %zu written, %zu unchanged, %zu skipped; %zu rebuilt in part (code "
                  "shared with another system software version)\n",
                  manifest.c_str(), st.written, st.unchanged, st.skipped, st.partial);
      return 0;
    }
    std::fprintf(stderr, "bad arguments for '%s'\n", cmd.c_str());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
