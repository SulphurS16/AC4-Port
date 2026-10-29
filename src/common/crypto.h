// Thin wrappers over Windows CNG (bcrypt): the hashes that check the player's files. All
// functions throw std::runtime_error on failure.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace ac4::crypto {

std::array<u8, 20> sha1(const void* data, size_t len);
std::array<u8, 32> sha256(const void* data, size_t len);
std::string sha256_file_hex(const std::string& path);

}  // namespace ac4::crypto
