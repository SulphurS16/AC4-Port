// Run logging: one log file per run plus warnings/errors mirrored to stderr.
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

void log_open(const std::string& path);
void log_set_level(Level l);
Level log_level();
void log_write(Level l, const char* chan, const std::string& msg);
void log_flush();
// Seconds since the log's time origin (the time stamps of log lines).
double log_time();

#define LOGF(lvl, chan, ...)                                                       \
  do {                                                                             \
    if ((lvl) >= ::rt::log_level()) ::rt::log_write((lvl), (chan), ac4::strprintf(__VA_ARGS__)); \
  } while (0)
#define LOG_T(chan, ...) LOGF(::rt::Level::Trace, chan, __VA_ARGS__)
#define LOG_D(chan, ...) LOGF(::rt::Level::Debug, chan, __VA_ARGS__)
#define LOG_I(chan, ...) LOGF(::rt::Level::Info, chan, __VA_ARGS__)
#define LOG_W(chan, ...) LOGF(::rt::Level::Warn, chan, __VA_ARGS__)
#define LOG_E(chan, ...) LOGF(::rt::Level::Error, chan, __VA_ARGS__)

// Logs a fatal error and terminates the process (after flushing the log and running the fatal
// hook once: diagnostics reports that would otherwise be lost).
[[noreturn]] void fatal(const std::string& msg);
void set_fatal_hook(void (*hook)());

}  // namespace rt
