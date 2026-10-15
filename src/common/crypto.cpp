#include "common/crypto.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace ac4::crypto {
namespace {

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

void check(NTSTATUS st, const char* what) {
  if (!NT_SUCCESS(st)) throw std::runtime_error(strprintf("bcrypt %s failed: 0x%08lx", what, (unsigned long)st));
}

template <size_t N>
std::array<u8, N> hash_with(const wchar_t* algname, const void* data, size_t len) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  check(BCryptOpenAlgorithmProvider(&alg, algname, nullptr, 0), "open hash");
  std::array<u8, N> out{};
  NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)data, (ULONG)len, out.data(), (ULONG)N);
  BCryptCloseAlgorithmProvider(alg, 0);
  check(st, "hash");
  return out;
}

}  // namespace

std::array<u8, 20> sha1(const void* data, size_t len) { return hash_with<20>(BCRYPT_SHA1_ALGORITHM, data, len); }
std::array<u8, 32> sha256(const void* data, size_t len) { return hash_with<32>(BCRYPT_SHA256_ALGORITHM, data, len); }

std::string sha256_file_hex(const std::string& path) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "open sha256");
  check(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0), "create hash");
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), (std::streamsize)buf.size());
    std::streamsize n = f.gcount();
    if (n > 0) check(BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)n, 0), "hash data");
  }
  u8 out[32];
  check(BCryptFinishHash(h, out, 32, 0), "finish hash");
  BCryptDestroyHash(h);
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex(out, 32);
}

}  // namespace ac4::crypto
