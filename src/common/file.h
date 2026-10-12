// File helpers. Every write goes through guard_write(), which refuses paths inside the
// protected read-only roots (the shared game-input tree and the reference projects).
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace ac4 {

namespace fs = std::filesystem;

// Adds a root that must never be written (compared case-insensitively after canonicalization).
void add_protected_root(const fs::path& p);
// Throws if `p` resolves inside any protected root.
void guard_write(const fs::path& p);

std::vector<u8> read_file(const fs::path& p);
std::vector<u8> read_file_range(const fs::path& p, u64 offset, u64 size);
void write_file(const fs::path& p, const void* data, size_t size);  // creates parent dirs
inline void write_file(const fs::path& p, const std::vector<u8>& v) { write_file(p, v.data(), v.size()); }
void write_text(const fs::path& p, const std::string& s);

}  // namespace ac4
