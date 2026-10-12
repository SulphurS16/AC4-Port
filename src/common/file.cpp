#include "common/file.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace ac4 {
namespace {

std::mutex g_mu;
std::vector<std::wstring> g_protected = {
};

std::wstring norm(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
  std::wstring s = (ec ? fs::absolute(p) : abs).wstring();
  for (auto& c : s) {
    c = (wchar_t)std::towlower(c);
    if (c == L'/') c = L'\\';
  }
  while (s.size() > 3 && s.back() == L'\\') s.pop_back();
  return s;
}

}  // namespace

void add_protected_root(const fs::path& p) {
  std::lock_guard lk(g_mu);
  g_protected.push_back(norm(p));
}

void guard_write(const fs::path& p) {
  std::wstring n = norm(p);
  std::lock_guard lk(g_mu);
  for (const auto& r : g_protected) {
    if (n == r || (n.size() > r.size() && n.compare(0, r.size(), r) == 0 && n[r.size()] == L'\\'))
      throw std::runtime_error("refusing to write inside protected read-only tree: " + p.string());
  }
}

std::vector<u8> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + p.string());
  std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<u8> v(static_cast<size_t>(n));
  if (n && !f.read(reinterpret_cast<char*>(v.data()), n)) throw std::runtime_error("read failed: " + p.string());
  return v;
}

std::vector<u8> read_file_range(const fs::path& p, u64 offset, u64 size) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + p.string());
  f.seekg(std::streamoff(offset));
  std::vector<u8> v(static_cast<size_t>(size));
  if (size && !f.read(reinterpret_cast<char*>(v.data()), std::streamsize(size)))
    throw std::runtime_error("short read: " + p.string());
  return v;
}

void write_file(const fs::path& p, const void* data, size_t size) {
  guard_write(p);
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("cannot create " + p.string());
  if (size) f.write(static_cast<const char*>(data), std::streamsize(size));
  if (!f) throw std::runtime_error("write failed: " + p.string());
}

void write_text(const fs::path& p, const std::string& s) { write_file(p, s.data(), s.size()); }

}  // namespace ac4
