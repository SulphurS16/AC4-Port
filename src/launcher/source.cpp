#include "launcher/source.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>

#include "common/crypto.h"
#include "common/file.h"
#include "launcher/process.h"

namespace launcher {
namespace {

constexpr int kSourceZip = 2;                              // RT_RCDATA resource id (launcher_source.rc)
constexpr const wchar_t* kStamp = L".launcher-source";     // in the unpacked folder: the zip's SHA-256

struct Blob {
  const unsigned char* data = nullptr;
  size_t size = 0;
};

Blob embedded() {
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(kSourceZip), RT_RCDATA);
  HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
  const void* p = g ? LockResource(g) : nullptr;
  if (!p) return {};
  return {static_cast<const unsigned char*>(p), size_t(SizeofResource(nullptr, r))};
}

std::string blob_id(const Blob& b) {
  auto h = ac4::crypto::sha256(b.data, b.size);
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  for (unsigned char x : h) {
    s += kHex[x >> 4];
    s += kHex[x & 15];
  }
  return s;
}

std::string read_line(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::string s;
  std::getline(f, s);
  return s;
}

}  // namespace

bool has_embedded_source() { return embedded().size != 0; }

bool source_current(const fs::path& dir) {
  Blob b = embedded();
  std::error_code ec;
  return b.size && read_line(dir / kStamp) == blob_id(b) && fs::is_regular_file(dir / "CMakeLists.txt", ec);
}

std::string unpack_source(const fs::path& dir) {
  Blob b = embedded();
  if (!b.size) return "This launcher holds no port sources: put it in the port's folder (the one with CMakeLists.txt, data and src).";
  if (source_current(dir)) return "";
  std::error_code ec;
  // Only the launcher's own earlier copy is ever replaced.
  if (fs::exists(dir, ec) && !fs::is_empty(dir, ec) && !fs::exists(dir / kStamp, ec))
    return narrow(dir.wstring()) + " exists and was not made by the launcher: choose another install folder (More > Install folder).";
  const std::string failed = "The port's sources could not be unpacked into " + narrow(dir.wstring());
  fs::path zip = dir.parent_path() / "source.zip.tmp", fresh = dir.parent_path() / "source.new";
  fs::remove_all(fresh, ec);
  try {
    ac4::guard_write(dir);
    ac4::write_file(zip, b.data, b.size);
    fs::create_directories(fresh);
  } catch (const std::exception& e) {
    fs::remove(zip, ec);
    return failed + ": " + e.what();
  }
  // Windows' own tar (Windows 10 1803 and later) unpacks zip archives, as it does the build tools.
  // -m: the files get the time of unpacking, newer than any earlier build's outputs, so the build
  // redoes what depends on them (the archive's own times are fixed, for a reproducible launcher).
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  fs::path tar = fs::path(sys) / L"tar.exe";
  std::wstring cmd = quote_arg(tar.wstring()) + L" -xmf " + quote_arg(zip.wstring()) + L" -C " + quote_arg(fresh.wstring());
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  DWORD code = 1;
  if (CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fresh.c_str(), &si, &pi)) {
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  fs::remove(zip, ec);
  if (code != 0 || !fs::is_regular_file(fresh / "CMakeLists.txt", ec)) {
    fs::remove_all(fresh, ec);
    return failed + ".";
  }
  try {
    ac4::write_text(fresh / kStamp, blob_id(b) + "\n");
  } catch (const std::exception& e) {
    fs::remove_all(fresh, ec);
    return failed + ": " + e.what();
  }
  fs::remove_all(dir, ec);
  fs::rename(fresh, dir, ec);
  if (ec) {
    fs::remove_all(fresh, ec);
    return failed + ".";
  }
  return "";
}

}  // namespace launcher
