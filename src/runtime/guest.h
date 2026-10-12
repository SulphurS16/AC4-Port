// Helpers for HLE code that reads/writes guest (big-endian) memory and structures.
#pragma once
#include <string>
#include <type_traits>

#include "ppu/ppu_recomp.h"

namespace rt {

template <typename T>
inline T bswap_any(T v) {
  static_assert(std::is_trivially_copyable_v<T>);
  if constexpr (sizeof(T) == 1) {
    return v;
  } else if constexpr (sizeof(T) == 2) {
    u16 x;
    std::memcpy(&x, &v, 2);
    x = __builtin_bswap16(x);
    std::memcpy(&v, &x, 2);
    return v;
  } else if constexpr (sizeof(T) == 4) {
    u32 x;
    std::memcpy(&x, &v, 4);
    x = __builtin_bswap32(x);
    std::memcpy(&v, &x, 4);
    return v;
  } else {
    static_assert(sizeof(T) == 8);
    u64 x;
    std::memcpy(&x, &v, 8);
    x = __builtin_bswap64(x);
    std::memcpy(&v, &x, 8);
    return v;
  }
}

// Big-endian scalar stored in guest memory layout.
template <typename T>
struct be {
  T raw;
  be() = default;
  be(T v) : raw(bswap_any(v)) {}
  operator T() const { return bswap_any(raw); }
  T value() const { return bswap_any(raw); }
  be& operator=(T v) {
    raw = bswap_any(v);
    return *this;
  }
  be& operator+=(T v) { return *this = value() + v; }
  be& operator-=(T v) { return *this = value() - v; }
  be& operator|=(T v) { return *this = value() | v; }
  be& operator&=(T v) { return *this = value() & v; }
};
using be16 = be<u16>;
using be32 = be<u32>;
using be64 = be<u64>;
using bes32 = be<s32>;
using bes64 = be<s64>;
using bef32 = be<float>;

// 32-bit guest pointer to T (T is a host-layout struct of be<> fields or a scalar).
template <typename T>
struct Ptr {
  u32 addr = 0;
  Ptr() = default;
  explicit Ptr(u32 a) : addr(a) {}
  T* get() const { return reinterpret_cast<T*>(ppu::mem() + addr); }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }
  T& operator[](size_t i) const { return get()[i]; }
  explicit operator bool() const { return addr != 0; }
};

template <typename T>
inline T* gp(u32 addr) {
  return reinterpret_cast<T*>(ppu::mem() + addr);
}

std::string read_cstr(u32 addr, size_t maxlen = 4096);
void write_cstr(u32 addr, const std::string& s, size_t bufsize);  // always NUL-terminates

}  // namespace rt
