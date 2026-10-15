// Child processes for the launcher: output captured line by line, the whole process tree ended
// on cancel (job object), PATH extended for the project-local toolchain.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace launcher {

struct RunResult {
  bool started = false;
  bool cancelled = false;
  unsigned long exit_code = 0;
  std::string error;  // why it did not start
};

class Runner {
 public:
  Runner();
  ~Runner();
  // Runs exe with args in cwd; PATH gets path_prefix in front. Each output line (stdout and
  // stderr, UTF-8) goes to on_line. Blocks until the process ends or cancel() is called.
  RunResult run(const std::filesystem::path& exe, const std::vector<std::wstring>& args, const std::filesystem::path& cwd,
                const std::wstring& path_prefix, const std::function<void(const std::string&)>& on_line);
  void cancel();  // any thread: ends the running process tree
  bool cancelled() const { return cancelled_.load(); }
  void reset() { cancelled_ = false; }

 private:
  void* job_ = nullptr;
  std::atomic<bool> cancelled_{false};
};

std::wstring quote_arg(const std::wstring& a);  // Windows command-line quoting
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

}  // namespace launcher
