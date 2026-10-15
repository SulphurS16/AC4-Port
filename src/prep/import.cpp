// Keyless preparation, the player path: files the player decrypted (for example with RPCS3's
// Utilities > Decrypt PS3 Binaries) and the plain files that come with them, each verified by SHA-256 against
// common/known_inputs.h, become the prepared inputs of the recompilers and the runtime:
// eboot.elf, lle/<module>.elf (relocated to their fixed slots), the dev_flash file the game reads
// and, when given, the update's game data. Nothing is decrypted here.
#include <cstdio>

#include "common/known_inputs.h"
#include "prep/formats.h"

namespace ac4::prep {

namespace {

namespace ki = ac4::inputs;

// Checks `p` against a known file; the reason names the file, where it was looked for and what is
// wrong with it.
std::string verify(const fs::path& p, const ki::Known& k, const std::string& what) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) throw std::runtime_error(what + " not found: " + p.string());
  std::string h = crypto::sha256_file_hex(p.string());
  if (h != k.sha256)
    throw std::runtime_error(strprintf("%s is not the expected file (%llu bytes, sha256 %s; expected %llu bytes, sha256 %s): %s",
                                       what.c_str(), (unsigned long long)fs::file_size(p, ec), h.c_str(),
                                       (unsigned long long)k.size, k.sha256, p.string().c_str()));
  return h;
}

void copy_verified(const fs::path& from, const fs::path& to) {
  std::vector<u8> d = read_file(from);
  write_file(to, d);
}

}  // namespace

std::string import_inputs(const ImportInputs& in, const fs::path& out, const fs::path& flash_out) {
  for (const fs::path& p : {in.disc, in.eboot_elf.parent_path(), in.modules_dir, in.flash_atrac.parent_path(), in.game_data})
    if (!p.empty()) add_protected_root(p);  // inputs are only read
  std::string m = "# ac4-prep manifest (keyless import of the player's decrypted files; machine-local)\n";

  // The disc folder: read in place by the runtime for the game's assets.
  fs::path sfo_path = in.disc / "PS3_GAME" / "PARAM.SFO";
  std::error_code ec;
  if (!fs::is_regular_file(sfo_path, ec)) throw std::runtime_error("not a PS3 disc folder (PS3_GAME/PARAM.SFO missing): " + in.disc.string());
  auto sfo = parse_sfo(read_file(sfo_path));
  auto str = [&](const char* k) {
    auto it = sfo.find(k);
    return it != sfo.end() && std::holds_alternative<std::string>(it->second) ? std::get<std::string>(it->second) : std::string();
  };
  if (str("TITLE_ID") != ki::kTitleId)
    throw std::runtime_error("the disc folder holds " + str("TITLE_ID") + ", not ARMORED CORE for Answer (USA, BLUS30187)");
  if (str("VERSION") != ki::kDiscVersion)
    throw std::runtime_error("the disc folder is disc version " + str("VERSION") + "; the port supports disc version 01.01");
  m += "[disc PARAM.SFO]\n" + sfo_to_text(sfo);

  // The executable.
  std::string eh = verify(in.eboot_elf, ki::kEbootElf, "the decrypted executable (EBOOT.elf)");
  copy_verified(in.eboot_elf, out / "eboot.elf");
  m += strprintf("[inputs]\nEBOOT.elf sha256=%s\n", eh.c_str());
  m += strprintf("[outputs]\neboot.elf size=%llu sha256=%s\n", (unsigned long long)ki::kEbootElf.size, eh.c_str());

  // The system modules: relocated to their slots, as the recompiler and runtime expect them.
  std::string lle;
  for (int i = 0; i < ki::kModuleCount; ++i) {
    std::string name = ki::kModules[i];
    const ki::Known* k = ki::module(name);
    fs::path p = in.modules_dir / k->name;
    if (!fs::is_regular_file(p, ec)) throw std::runtime_error("the decrypted system module " + std::string(k->name) + " not found: " + p.string());
    std::string h = crypto::sha256_file_hex(p.string());
    if (h != k->sha256) {
      std::string other = ki::module_other_version(name, h);
      throw std::runtime_error(other.empty() ? strprintf("%s is not the expected decrypted module of system software %s (sha256 %s): %s", k->name,
                                                         ki::kSystemSoftware, h.c_str(), p.string().c_str())
                                             : strprintf("%s comes from system software %s; the port needs %s: %s", k->name, other.c_str(),
                                                         ki::kSystemSoftware, p.string().c_str()));
    }
    u32 base = kLleBase + u32(i) * kLleSlot;
    std::string log;
    std::vector<u8> img = relocate_prx(read_file(p), base, &log);
    write_file(out / "lle" / (name + ".elf"), img);
    auto ih = crypto::sha256(img.data(), img.size());
    lle += strprintf("%s base=0x%08x prx_sha256=%s elf_sha256=%s\n", name.c_str(), base, h.c_str(), hex(ih.data(), 32).c_str());
  }
  write_text(out / "lle" / "manifest.txt", lle);
  m += "[system modules " + std::string(ki::kSystemSoftware) + "]\n" + lle;

  // The one dev_flash file the game reads.
  std::string fh = verify(in.flash_atrac, ki::kFlashAtrac, "flashATRAC.pic");
  copy_verified(in.flash_atrac, flash_out / "sys" / "external" / ki::kFlashAtrac.name);
  m += strprintf("[dev_flash]\nsys/external/flashATRAC.pic sha256=%s\n", fh.c_str());

  // The update's game data (optional): only the given data is installed.
  fs::remove_all(out / "update", ec);
  if (!in.game_data.empty()) {
    fs::path reg = in.game_data / "USRDIR" / ki::kRegulation.name;  // the update's folder
    if (!fs::is_regular_file(reg, ec)) reg = in.game_data / ki::kRegulation.name;  // or regulation.bin on its own
    std::string rh = verify(reg, ki::kRegulation, "the update's regulation.bin");
    fs::path dst = out / "update" / ki::kUpdateContentId;
    copy_verified(reg, dst / "USRDIR" / ki::kRegulation.name);
    m += strprintf("[update %s]\nUSRDIR/regulation.bin sha256=%s\n", ki::kUpdateContentId, rh.c_str());
    for (const char* f : ki::kUpdateFiles) {
      fs::path src = in.game_data / f;
      if (!fs::is_regular_file(src, ec)) continue;
      copy_verified(src, dst / f);
      m += strprintf("%s sha256=%s\n", f, crypto::sha256_file_hex(src.string()).c_str());
    }
  }
  write_text(out / "manifest.txt", m);
  return m;
}

}  // namespace ac4::prep
