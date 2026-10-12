#include "runtime/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "common/file.h"

namespace rt {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
std::atomic<Level> g_level{Level::Info};
const auto g_t0 = std::chrono::steady_clock::now();
const char* name(Level l) {
  switch (l) {
    case Level::Trace: return "T";
    case Level::Debug: return "D";
    case Level::Info: return "I";
    case Level::Warn: return "W";
    case Level::Error: return "E";
    case Level::Fatal: return "F";
  }
  return "?";
}
}  // namespace

void log_open(const std::string& path) {
  ac4::guard_write(path);  // throws inside a protected root
  std::lock_guard lk(g_mu);
  if (g_file) std::fclose(g_file);
  g_file = std::fopen(path.c_str(), "wb");
}
void log_set_level(Level l) { g_level = l; }
Level log_level() { return g_level; }

double log_time() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }

void log_write(Level l, const char* chan, const std::string& msg) {
  double t = log_time();
  char head[96];
  std::snprintf(head, sizeof head, "[%9.4f %s %5lu %s] ", t, name(l), GetCurrentThreadId() % 100000, chan);
  std::lock_guard lk(g_mu);
  if (g_file) {
    std::fputs(head, g_file);
    std::fputs(msg.c_str(), g_file);
    std::fputc('\n', g_file);
    if (l >= Level::Warn) std::fflush(g_file);
  }
  if (l >= Level::Warn || !g_file) {
    std::fputs(head, stderr);
    std::fputs(msg.c_str(), stderr);
    std::fputc('\n', stderr);
  }
}

void log_flush() {
  std::lock_guard lk(g_mu);
  if (g_file) std::fflush(g_file);
  std::fflush(stderr);
}

namespace {
void (*g_fatal_hook)() = nullptr;
}
void set_fatal_hook(void (*hook)()) { g_fatal_hook = hook; }

void fatal(const std::string& msg) {
  log_write(Level::Fatal, "fatal", msg);
  log_flush();
  static std::atomic<bool> once{false};
  if (g_fatal_hook && !once.exchange(true)) {  // diagnostics reports (profile, watchpoints)
    g_fatal_hook();
    log_flush();
  }
  TerminateProcess(GetCurrentProcess(), 3);
  for (;;) {
  }
}

}  // namespace rt
