#include "launcher/inputs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <deque>
#include <vector>

#include "common/crypto.h"
#include "common/file.h"
#include "common/known_inputs.h"
#include "common/sfo.h"
#include "launcher/process.h"

namespace launcher {
namespace {

namespace ki = ac4::inputs;

bool is_file(const fs::path& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

std::string hash_of(const fs::path& p) {
  try {
    return ac4::crypto::sha256_file_hex(narrow(p.wstring()));
  } catch (const std::exception&) {
    return "";
  }
}

std::string join(const std::vector<std::string>& v, size_t max_shown) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < max_shown; ++i) s += (i ? ", " : "") + v[i];
  if (v.size() > max_shown) s += ", ...";
  return s;
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = wchar_t(towlower(c));
  return s;
}

// The files named `name` (any case) in `root` and the folders below it, nearest first: a few
// folder levels, a bounded number of folders (the player may choose a parent folder).
std::vector<fs::path> find_named(const fs::path& root, const std::wstring& name) {
  std::vector<fs::path> out;
  if (root.empty()) return out;
  const std::wstring want = lower(name);
  std::deque<std::pair<fs::path, int>> todo{{root, 0}};
  int folders = 0;
  while (!todo.empty() && folders < 4000) {
    auto [dir, depth] = todo.front();
    todo.pop_front();
    ++folders;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_directory(e2)) {
        if (depth < 4 && !it->is_symlink(e2)) todo.push_back({it->path(), depth + 1});
      } else if (lower(it->path().filename().wstring()) == want) {
        out.push_back(it->path());
      }
    }
  }
  return out;
}

// The candidate whose content is the expected file, else the first one.
fs::path pick(const std::vector<fs::path>& candidates, const ki::Known& k) {
  for (const fs::path& p : candidates) {
    std::error_code ec;
    if (fs::file_size(p, ec) == k.size && hash_of(p) == k.sha256) return p;
  }
  return candidates.empty() ? fs::path() : candidates.front();
}

}  // namespace

Found find_files(const fs::path& disc, const fs::path& files) {
  Found f;
  // EBOOT.elf: decrypting EBOOT.BIN usually puts it next to it, in the game folder.
  std::vector<fs::path> eboots;
  if (!disc.empty() && is_file(disc / "PS3_GAME" / "USRDIR" / "EBOOT.elf")) eboots.push_back(disc / "PS3_GAME" / "USRDIR" / "EBOOT.elf");
  for (const fs::path& p : find_named(files, L"EBOOT.elf")) eboots.push_back(p);
  f.eboot_elf = pick(eboots, ki::kEbootElf);
  // The modules: the folder of the decrypted libsre.prx (the 10 are decrypted together).
  const ki::Known* sre = ki::module("libsre");
  fs::path prx = pick(find_named(files, L"libsre.prx"), *sre);
  if (!prx.empty()) {
    f.modules_dir = prx.parent_path();
  } else {
    std::vector<fs::path> sprx = find_named(files, L"libsre.sprx");
    if (!sprx.empty()) {
      f.modules_dir = sprx.front().parent_path();
      f.encrypted_modules = true;
    }
  }
  return f;
}

Status check_disc(const fs::path& disc, const Found& f) {
  if (disc.empty()) return {Verdict::Empty, "Choose the game's folder (the one with PS3_GAME).", ""};
  fs::path sfo = disc / "PS3_GAME" / "PARAM.SFO";
  if (!is_file(sfo)) return {Verdict::Bad, "Not a PS3 game folder: PS3_GAME\\PARAM.SFO is missing.", ""};
  std::string id, version;
  try {
    ac4::sfo::Map m = ac4::sfo::parse(ac4::read_file(sfo));
    id = ac4::sfo::get_string(m, "TITLE_ID");
    version = ac4::sfo::get_string(m, "VERSION");
  } catch (const std::exception& e) {
    return {Verdict::Bad, std::string("PARAM.SFO unreadable: ") + e.what(), ""};
  }
  if (id != ki::kTitleId)
    return {Verdict::Bad, "This is " + id + ", not " + ki::kTitleName + " (USA, " + ki::kTitleId + ").", ""};
  if (version != ki::kDiscVersion)
    return {Verdict::Bad, std::string(ki::kTitleId) + " disc " + version + ": the port supports disc " + ki::kDiscVersion + ".", ""};
  if (f.eboot_elf.empty())
    return {Verdict::Bad, "EBOOT.elf not found: decrypt PS3_GAME\\USRDIR\\EBOOT.BIN.",
            "EBOOT.elf found neither in " + narrow((disc / "PS3_GAME" / "USRDIR").wstring()) + " nor in the decrypted files' folder"};
  std::string h = hash_of(f.eboot_elf);
  if (h != ki::kEbootElf.sha256)
    return {Verdict::Bad, "EBOOT.elf is not the decrypted executable of disc 01.01 (decrypt EBOOT.BIN again).",
            narrow(f.eboot_elf.wstring()) + ": sha256 " + h + ", expected " + ki::kEbootElf.sha256};
  return {Verdict::Ok, std::string(ki::kTitleName) + " (" + ki::kTitleId + ", disc " + version + "), EBOOT.elf", ""};
}

Status check_modules(const fs::path& files, const Found& f) {
  if (files.empty()) return {Verdict::Empty, "Choose the folder with the decrypted files.", ""};
  if (f.modules_dir.empty())
    return {Verdict::Bad, "The decrypted system modules (.prx) are not in this folder.", ""};
  if (f.encrypted_modules)
    return {Verdict::Bad, "The system modules here are still encrypted (.sprx): decrypt them.",
            "only " + narrow((f.modules_dir / "libsre.sprx").wstring()) + " and the other .sprx files"};
  std::vector<std::string> missing, other, wrong;
  std::string other_version, details;
  for (int i = 0; i < ki::kModuleCount; ++i) {
    const ki::Known* k = ki::module(ki::kModules[i]);
    fs::path p = f.modules_dir / k->name;
    if (!is_file(p)) {
      missing.push_back(k->name);
      details += std::string(k->name) + ": not found in " + narrow(f.modules_dir.wstring()) + "\n";
      continue;
    }
    std::string h = hash_of(p);
    if (h == k->sha256) continue;
    std::string v = ki::module_other_version(ki::kModules[i], h);
    if (!v.empty()) {
      other.push_back(k->name);
      other_version = v;
      details += std::string(k->name) + ": from system software " + v + "\n";
    } else {
      wrong.push_back(k->name);
      details += std::string(k->name) + ": sha256 " + h + ", expected " + k->sha256 + "\n";
    }
  }
  fs::path pic = f.modules_dir / ki::kFlashAtrac.name;
  bool pic_ok = is_file(pic) && hash_of(pic) == ki::kFlashAtrac.sha256;
  if (!pic_ok) details += std::string(ki::kFlashAtrac.name) + (is_file(pic) ? ": differs from every known version\n" : ": not found\n");
  if (!other.empty())
    return {Verdict::Bad, "These modules are from system software " + other_version + "; the port needs " + ki::kSystemSoftware + ".", details};
  if (!missing.empty())
    return {Verdict::Bad, "Missing: " + join(missing, 3) + " (" + std::to_string(missing.size()) + " of 10 system modules).", details};
  if (!wrong.empty()) return {Verdict::Bad, "Not the expected decrypted modules: " + join(wrong, 3) + ".", details};
  if (!pic_ok) return {Verdict::Bad, "flashATRAC.pic is missing or damaged (it belongs next to the modules).", details};
  return {Verdict::Ok, "The 10 system modules of system software " + std::string(ki::kSystemSoftware), ""};
}

std::wstring files_help() {
  std::wstring mods;
  for (int i = 0; i < ki::kModuleCount; ++i) mods += (i ? L", " : L"") + widen(ki::kModules[i]);
  return L"The port contains no keys and decrypts nothing. It needs these files, decrypted by you (for example with RPCS3: "
         L"Utilities > Decrypt PS3 Binaries):\n\n"
         L"\u2022 EBOOT.elf: the game's PS3_GAME\\USRDIR\\EBOOT.BIN, decrypted. It can stay next to EBOOT.BIN.\n\n"
         L"\u2022 The 10 system modules of PS3 system software " + widen(ki::kSystemSoftware) + L" (sys\\external), decrypted to .prx: " + mods +
         L"; with flashATRAC.pic from the same folder.\n\n"
         L"Choose the folder that holds them (they may also be in folders below it), or let the launcher prepare them: Prepare "
         L"files asks for your RPCS3 and the PS3 system software 4.93 update file (PS3UPDAT.PUP from playstation.com).";
}

}  // namespace launcher
