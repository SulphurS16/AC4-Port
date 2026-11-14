// The player's game files in the keyless setup (docs/INPUTS.md): the game's disc folder
// (BLUS30027, disc VERSION 01.01, read in place) and files the player decrypted themselves (for
// example with RPCS3's Utilities > Decrypt PS3 Binaries): EBOOT.elf (from PS3_GAME\USRDIR\
// EBOOT.BIN) and the 10 system modules of PS3 system software 4.93 as .prx with flashATRAC.pic
// from the same folder. Each file is accepted only with its SHA-256 (common/known_inputs.h); the
// port contains no keys and decrypts nothing.
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
// folder; the modules and flashATRAC.pic in that folder or a folder below it.
// Empty paths: not found.
struct Found {
  fs::path eboot_elf;    // EBOOT.elf
  fs::path modules_dir;  // the folder holding the .prx files and flashATRAC.pic
  bool encrypted_modules = false;  // only the encrypted .sprx files were found
};
Found find_files(const fs::path& disc, const fs::path& files);

Status check_disc(const fs::path& disc, const Found& f);    // the disc and its decrypted EBOOT.elf
// The Game update row (optional): AC4 has no update package; its updates were regulation files the
// game downloaded while online (into its game data, USRDIR\regulation.bin). The row takes such a
// file (or a community regulation): Ok for a regulation of the game (a BND3 archive, version
// "verNNN" in its header), Bad for another file, Empty without one (the disc's Ver.1.00).
Status check_regulation(const fs::path& file);
Status check_modules(const fs::path& files, const Found& f);  // the 10 modules and flashATRAC.pic

// The files the port needs and where they come from (shown when some are missing).
std::wstring files_help();

}  // namespace launcher
