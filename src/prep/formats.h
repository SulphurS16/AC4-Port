// Local preparation: PARAM.SFO, PRX relocation, the keyless import of the player's decrypted files
// and SPU code records; in development builds also the key-based path (retail PKG, SELF, system
// software), which the release does not contain.
#pragma once
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/bytes.h"
#include "common/crypto.h"
#include "common/file.h"

namespace ac4::prep {

// ---- PARAM.SFO (little-endian) ----
using SfoValue = std::variant<std::string, u32>;
std::map<std::string, SfoValue> parse_sfo(const std::vector<u8>& data);
std::string sfo_to_text(const std::map<std::string, SfoValue>& sfo);

// Relocates a decrypted PRX to a fixed base; returns a position-dependent ELF image.
std::vector<u8> relocate_prx(const std::vector<u8>& prx, u32 base, std::string* log);
// Module i at kLleBase + i * kLleSlot. AC4's executable maps 0x10000-0xF0C000 and
// 0x10000000-0x15DE3000 (SPU-shared segments), so the slots lie between them (up to 22 modules).
constexpr u32 kLleBase = 0x0A000000, kLleSlot = 0x00400000;

// ---- keyless import (import.cpp): the player's files, verified by SHA-256 ----
struct ImportInputs {
  fs::path disc;         // the disc folder (PS3_GAME/PARAM.SFO is checked; assets are read in place)
  fs::path eboot_elf;    // EBOOT.elf (decrypted PS3_GAME/USRDIR/EBOOT.BIN)
  fs::path modules_dir;  // <module>.prx (the decrypted modules of sys/external)
  fs::path flash_atrac;  // flashATRAC.pic (from the same sys/external folder)
  fs::path game_data;    // optional: the update's folder (USRDIR/regulation.bin) or a folder with regulation.bin
};
// Writes eboot.elf, lle/*.elf, the update data and manifest.txt to `out` and the dev_flash file
// below `flash_out`; returns the manifest. Throws with a precise reason on a missing or wrong file.
std::string import_inputs(const ImportInputs& in, const fs::path& out, const fs::path& flash_out);

// ---- SPU code records from the SPU code manifest (spu_records.cpp) ----
struct SpuRecordStats {
  size_t written = 0, unchanged = 0, skipped = 0;
  size_t partial = 0;  // rebuilt without the ranges of another input version
  std::string notes;  // sources missing or with other hashes
};
SpuRecordStats rebuild_spu_records(const fs::path& manifest, const fs::path& prepared, const fs::path& out_dir);

}  // namespace ac4::prep
