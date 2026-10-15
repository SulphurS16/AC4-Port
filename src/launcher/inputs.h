// The player's game files in the keyless setup (docs/INPUTS.md): the game's disc folder
// (BLUS30187, disc VERSION 01.01, read in place) and files the player decrypted themselves (for
// example with RPCS3's Utilities > Decrypt PS3 Binaries): EBOOT.elf (from PS3_GAME\USRDIR\
// EBOOT.BIN), the 10 system modules of PS3 system software 4.93 as .prx with flashATRAC.pic from
// the same folder, optionally regulation.bin of the update 01.35. Each file is accepted only with
// its SHA-256 (common/known_inputs.h); the port contains no keys and decrypts nothing.
#pragma once
#include <filesystem>
#include <string>

namespace launcher {

namespace fs = std::filesystem;

enum class Verdict { Empty, Ok, Optional, Bad };
struct Status {
  Verdict verdict = Verdict::Empty;
  std::string text;     // one line for the window
  std::string details;  // every problem, for the log
};

// Where the files are: EBOOT.elf next to EBOOT.BIN in the game folder or in the decrypted files'
// folder; the modules, flashATRAC.pic and regulation.bin in that folder or a folder below it.
// Empty paths: not found.
struct Found {
  fs::path eboot_elf;    // EBOOT.elf
  fs::path modules_dir;  // the folder holding the .prx files and flashATRAC.pic
  fs::path regulation;   // the update's regulation.bin, when present
  bool encrypted_modules = false;  // only the encrypted .sprx files were found
};
Found find_files(const fs::path& disc, const fs::path& files);

Status check_disc(const fs::path& disc, const Found& f);    // the disc and its decrypted EBOOT.elf
Status check_modules(const fs::path& files, const Found& f);  // the 10 modules and flashATRAC.pic
Status check_update(const Found& f);                          // optional

// The files the port needs and where they come from (shown when some are missing).
std::wstring files_help();

}  // namespace launcher
