// The player's inputs in the keyless setup (docs/INPUTS.md): the disc folder (read in place), files
// the player decrypted (for example with RPCS3's Utilities > Decrypt PS3 Binaries) and the plain
// files that come with them. Each file is accepted only with its SHA-256; the port holds no keys
// and decrypts nothing.
#pragma once
#include <string>

#include "common/bytes.h"

namespace ac4::inputs {

struct Known {
  const char* name;    // file name as RPCS3 writes it
  const char* sha256;  // lower-case hex
  u64 size;
};

// The disc (VERSION 01.01): its PARAM.SFO and the still-encrypted executable it carries.
constexpr const char* kTitleId = "BLUS30187";
constexpr const char* kDiscVersion = "01.01";
extern const Known kEbootBin;  // PS3_GAME/USRDIR/EBOOT.BIN (disc)
// The decrypted executable: RPCS3 writes EBOOT.elf next to EBOOT.BIN.
extern const Known kEbootElf;

// The system modules run natively, in load order (each gets the next fixed address slot), as
// decrypted from PS3 system software 4.93: RPCS3 writes <name>.prx next to <name>.sprx in
// dev_flash/sys/external.
constexpr const char* kSystemSoftware = "4.93";
constexpr int kModuleCount = 10;
extern const char* const kModules[kModuleCount];
// The accepted 4.93 decryption of a module (nullptr: not one of kModules).
const Known* module(const std::string& name);
// The system software version a decrypted module comes from when it is not the accepted one, as
// far as known ("4.89"); "" when unknown.
std::string module_other_version(const std::string& name, const std::string& sha256);

// dev_flash/sys/external/flashATRAC.pic: not encrypted, the same in every version; the ATRAC3plus
// decoder reads it at run time.
extern const Known kFlashAtrac;

// The optional update 01.35 (regulation data): its installed folder (dev_hdd0/game/BLUS30187 of an
// emulator) or regulation.bin on its own.
extern const Known kRegulation;      // USRDIR/regulation.bin
constexpr const char* kUpdateContentId = "UP0001-BLUS30187_00-AC45000004141906";
constexpr const char* kUpdateFiles[] = {"PARAM.SFO", "ICON0.PNG", "PS3LOGO.DAT"};  // copied when present

}  // namespace ac4::inputs
