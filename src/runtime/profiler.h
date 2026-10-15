// Sampling profiler for host threads (diagnostics, --profile <thread-name-substring>[,...]). A sampler
// thread periodically suspends matching threads, records their instruction pointer as an offset
// from the executable's base, and writes "rva count" lines to profile.txt in the run directory
// (symbolize with llvm-addr2line -f -C -e ac4.exe).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace rt::prof {

// Player builds: no profiler.
inline void register_thread(const std::string&, void*) {}
inline void unregister_thread(void*) {}
}  // namespace rt::prof
