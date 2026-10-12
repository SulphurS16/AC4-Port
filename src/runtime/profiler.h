// Sampling profiler for host threads (diagnostics, --profile <thread-name-substring>[,...]). A sampler
// thread periodically suspends matching threads, records their instruction pointer as an offset
// from the executable's base, and writes "rva count" lines to profile.txt in the run directory
// (symbolize with llvm-addr2line -f -C -e ac4.exe).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace rt::prof {

// Registers a host thread (HANDLE) that may be sampled.
void register_thread(const std::string& name, void* handle);
void unregister_thread(void* handle);
// Snapshot of the registered threads (handle, name).
std::vector<std::pair<void*, std::string>> threads();
void start(const std::string& match);
void write_report(const std::string& path);
// Logs the CPU time (user + kernel) used so far by each registered thread, busiest first.
void cpu_report();
// The `top` busiest registered threads since the previous call, as "name NN%, ..." (percent of
// one core over the interval).
std::string cpu_interval(int top);

}  // namespace rt::prof
