// First-time preparation and build, run by the launcher on a worker thread: the pinned toolchain,
// the preparation tools, the player's game files prepared and translated, the game built, its
// shaders compiled. Each step records what it was made from (<data>\state\*.txt); a step whose
// record matches the current inputs is skipped, so an interrupted setup resumes and changed
// inputs redo only what depends on them. Output goes to <data>\launcher.log.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "launcher/process.h"

namespace launcher {

namespace fs = std::filesystem;

struct SetupConfig {
  fs::path src;   // the port's sources (CMakeLists.txt, data\)
  fs::path data;  // install folder: tools, prepared data, build, saves, settings
  fs::path disc;   // the game's disc folder (read in place; EBOOT.elf next to EBOOT.BIN)
  fs::path files;  // the folder with the decrypted files (inputs.h: in it or in folders below it)
};

enum class StepState { Waiting, Running, Done, Skipped, Failed };
struct StepView {
  std::string name;
  StepState state = StepState::Waiting;
};

struct SetupView {
  std::vector<StepView> steps;
  double fraction = 0;  // overall
  std::string status;   // current step's detail
  std::vector<std::string> lines;  // output since the last view
  bool running = false, finished = false, ok = false, cancelled = false;
  std::string error;
};

class Setup {
 public:
  Setup();
  ~Setup();
  void start(const SetupConfig& c, std::function<void()> notify);  // notify: from the worker, often
  void cancel();
  SetupView view();  // takes the new output lines
  bool running() const { return running_.load(); }

  // Whether everything is built from these inputs (Play possible).
  static bool ready(const SetupConfig& c);
  static fs::path game_exe(const SetupConfig& c);
  // The dev_flash folder the game reads (the one file it needs, flashATRAC.pic, imported).
  static fs::path flash_dir(const SetupConfig& c);
  // Disk space the setup still needs in the install folder (bytes, estimate).
  static unsigned long long space_needed(const SetupConfig& c);

 private:
  void run(SetupConfig c);
  void log(const std::string& line);
  void set_step(size_t i, StepState s);
  void set_fraction(double step_fraction);
  void set_status(const std::string& s);

  std::mutex mu_;
  SetupView view_;
  std::function<void()> notify_;
  std::thread worker_;
  std::atomic<bool> running_{false}, cancel_{false};
  Runner runner_;
  size_t step_ = 0;
  FILE* log_file_ = nullptr;
};

}  // namespace launcher
