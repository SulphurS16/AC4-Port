#include "launcher/setup.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "common/crypto.h"
#include "common/known_inputs.h"
#include "common/privacy.h"
#include "launcher/download.h"
#include "launcher/inputs.h"

namespace launcher {
namespace {

namespace ki = ac4::inputs;

struct StepDef {
  const char* name;
  double weight;  // share of the whole setup's time (rough)
};
enum : size_t { kToolchain, kTools, kImport, kRecompile, kSpu, kBuild, kShaders, kStepCount };
const StepDef kSteps[kStepCount] = {
    {"Download build tools", 12}, {"Build preparation tools", 6}, {"Import game files", 4}, {"Translate game code", 8},
    {"Translate SPU code", 5},    {"Build the game", 60},         {"Compile shaders", 5},
};

struct Tool {
  std::string name, sha, into, check, path, use, url;
};

std::vector<Tool> read_toolchain(const fs::path& src) {
  std::vector<Tool> out;
  std::ifstream f(src / "data" / "toolchain.txt");
  for (std::string line; std::getline(f, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream in(line);
    Tool t;
    if (in >> t.name >> t.sha >> t.into >> t.check >> t.path >> t.use >> t.url) out.push_back(t);
  }
  return out;
}

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}
bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

// What a file is, cheaply: size and modification time.
std::string file_stamp(const fs::path& p) {
  std::error_code ec;
  auto size = fs::file_size(p, ec);
  if (ec) return "none";
  auto t = fs::last_write_time(p, ec);
  return std::to_string((unsigned long long)size) + "@" + std::to_string((long long)t.time_since_epoch().count());
}

std::string content_hash(const fs::path& p) {
  if (!is_file(p)) return "none";
  try {
    return ac4::crypto::sha256_file_hex(narrow(p.wstring()));
  } catch (const std::exception&) {
    return "unreadable";
  }
}

fs::path prepared(const SetupConfig& c) { return c.data / "game" / ki::kTitleId; }
// The one dev_flash file the game reads (flashATRAC.pic), imported from the player's files.
fs::path flash(const SetupConfig& c) { return c.data / "game" / "dev_flash"; }
fs::path build_dir(const SetupConfig& c) { return c.data / "build"; }
fs::path tools_dir(const SetupConfig& c) { return c.data / "tools"; }
// Generated code lives inside the build folder: object files then get short relative paths there
// (sources elsewhere would repeat the whole install path in every object path).
fs::path gen_dir(const SetupConfig& c) { return build_dir(c) / "gen"; }

// Each step's record: what it was made from. A step is redone when its record differs.
std::string stamp(const SetupConfig& c, size_t step) {
  switch (step) {
    case kImport: {
      Found f = find_files(c.disc, c.files);
      std::string s = "disc " + c.disc.generic_string() + "\neboot " + file_stamp(f.eboot_elf) + "\n";
      for (int i = 0; i < ki::kModuleCount; ++i) s += std::string(ki::kModules[i]) + " " + file_stamp(f.modules_dir / ki::module(ki::kModules[i])->name) + "\n";
      return s + "flash " + file_stamp(f.modules_dir / ki::kFlashAtrac.name);
    }
    case kRecompile:
      return "prepared " + content_hash(prepared(c) / "manifest.txt") + "\nlle " + content_hash(prepared(c) / "lle" / "manifest.txt") +
             "\nhooks " + content_hash(c.src / "src" / "runtime" / "game" / "hooks.txt") + "\npatches " +
             content_hash(c.src / "src" / "runtime" / "game" / "patches.txt") + "\nrecompiler " +
             file_stamp(build_dir(c) / "ac4-recomp.exe");
    case kSpu:
      return "prepared " + content_hash(prepared(c) / "manifest.txt") + "\nlle " + content_hash(prepared(c) / "lle" / "manifest.txt") +
             "\nmanifest " + content_hash(c.src / "data" / "spu_manifest.txt") + "\nspurecomp " +
             file_stamp(build_dir(c) / "ac4-spurecomp.exe");
    default:
      return "";
  }
}

const char* state_name(size_t step) {
  switch (step) {
    case kImport: return "import";
    case kRecompile: return "recompile";
    case kSpu: return "spu";
    default: return "";
  }
}

std::string read_state(const SetupConfig& c, size_t step) {
  std::ifstream f(c.data / "state" / (std::string(state_name(step)) + ".txt"), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void write_state(const SetupConfig& c, size_t step, const std::string& s) {
  std::error_code ec;
  fs::create_directories(c.data / "state", ec);
  std::ofstream(c.data / "state" / (std::string(state_name(step)) + ".txt"), std::ios::binary) << s;
}

bool outputs_present(const SetupConfig& c, size_t step) {
  switch (step) {
    case kImport:
      for (const char* m : ki::kModules)
        if (!is_file(prepared(c) / "lle" / (std::string(m) + ".elf"))) return false;
      return is_file(prepared(c) / "eboot.elf") && is_file(prepared(c) / "manifest.txt") && is_file(prepared(c) / "lle" / "manifest.txt") &&
             is_file(flash(c) / "sys" / "external" / ki::kFlashAtrac.name);
    case kRecompile: return is_file(gen_dir(c) / "ppu" / "ppu_functab.cpp");
    case kSpu: return is_dir(gen_dir(c) / "spu");
    default: return false;
  }
}

bool step_current(const SetupConfig& c, size_t step) { return outputs_present(c, step) && read_state(c, step) == stamp(c, step); }

std::wstring tool_path_prefix(const SetupConfig& c) {
  std::wstring p;
  for (const Tool& t : read_toolchain(c.src))
    if (t.use == "build") p += (p.empty() ? L"" : L";") + (tools_dir(c) / widen(t.path)).wstring();
  return p;
}

fs::path cmake_exe(const SetupConfig& c) {
  for (const Tool& t : read_toolchain(c.src))
    if (t.name == "cmake") return tools_dir(c) / widen(t.path) / "cmake.exe";
  return "cmake.exe";
}

std::wstring w(const fs::path& p) { return p.wstring(); }
std::wstring fwd(const fs::path& p) { return p.generic_wstring(); }  // CMake prefers forward slashes

bool x86_64_v3() {
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx") && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") &&
         __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("sse4.2") &&
         __builtin_cpu_supports("popcnt");
}

unsigned long long free_space(fs::path p) {
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
  ULARGE_INTEGER avail{};
  if (p.empty() || !GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr)) return 0;
  return avail.QuadPart;
}

}  // namespace

Setup::Setup() {
  for (const StepDef& s : kSteps) view_.steps.push_back({s.name, StepState::Waiting});
}

Setup::~Setup() {
  cancel();
  if (worker_.joinable()) worker_.join();
  if (log_file_) std::fclose(log_file_);
}

fs::path Setup::game_exe(const SetupConfig& c) { return build_dir(c) / "ac4.exe"; }
fs::path Setup::flash_dir(const SetupConfig& c) { return flash(c); }

bool Setup::ready(const SetupConfig& c) {
  if (!is_file(game_exe(c))) return false;
  for (size_t s : {kImport, kRecompile, kSpu})
    if (!step_current(c, s)) return false;
  std::ifstream f(c.data / "state" / "built.txt", std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str() == stamp(c, kRecompile) + "\n" + stamp(c, kSpu);
}

unsigned long long Setup::space_needed(const SetupConfig& c) {
  // Measured on the development PC (2026-10-15): tools 0.92 GB (+0.25 GB of archives while
  // unpacking), prepared files 0.07, generated code 0.13, build 0.50, shaders and SPU code 0.01,
  // saves under 0.01; the game's system cache (cleared at each start, filled while playing) holds
  // copies of its bind\ archives: at most their 1.74 GB.
  const double gb = 1024.0 * 1024 * 1024;
  double need = 0;
  if (!is_dir(tools_dir(c) / "cmake-4.4.3-windows-x86_64")) need += 1.2;
  if (!is_file(prepared(c) / "eboot.elf")) need += 0.1;
  if (!is_file(gen_dir(c) / "ppu" / "ppu_functab.cpp")) need += 0.2;
  if (!is_file(game_exe(c))) need += 0.5;
  need += 1.8;  // the system cache (emptied at every start)
  return (unsigned long long)(need * gb);
}

void Setup::log(const std::string& line) {
  std::lock_guard lk(mu_);
  view_.lines.push_back(line);
  if (log_file_) {
    std::fputs(ac4::redact_personal(line).c_str(), log_file_);  // shared as it is
    std::fputc('\n', log_file_);
    std::fflush(log_file_);
  }
}

void Setup::set_step(size_t i, StepState s) {
  {
    std::lock_guard lk(mu_);
    view_.steps[i].state = s;
    if (s == StepState::Running) {
      step_ = i;
      view_.status = kSteps[i].name;
    }
  }
  if (s == StepState::Running) set_fraction(0);
}

void Setup::set_fraction(double f) {
  double total = 0, done = 0;
  for (size_t i = 0; i < kStepCount; ++i) {
    total += kSteps[i].weight;
    if (i < step_) done += kSteps[i].weight;
  }
  std::lock_guard lk(mu_);
  view_.fraction = (done + kSteps[step_].weight * std::min(1.0, std::max(0.0, f))) / total;
}

void Setup::set_status(const std::string& s) {
  std::lock_guard lk(mu_);
  view_.status = s;
}

SetupView Setup::view() {
  std::lock_guard lk(mu_);
  SetupView v = view_;
  view_.lines.clear();
  v.running = running_.load();
  return v;
}

void Setup::cancel() {
  cancel_ = true;
  runner_.cancel();
}

void Setup::start(const SetupConfig& c, std::function<void()> notify) {
  if (running_) return;
  if (worker_.joinable()) worker_.join();
  notify_ = std::move(notify);
  cancel_ = false;
  runner_.reset();
  {
    std::lock_guard lk(mu_);
    for (auto& s : view_.steps) s.state = StepState::Waiting;
    view_.fraction = 0;
    view_.status.clear();
    view_.lines.clear();
    view_.finished = view_.ok = view_.cancelled = false;
    view_.error.clear();
  }
  running_ = true;
  worker_ = std::thread([this, c] { run(c); });
}

void Setup::run(SetupConfig c) {
  std::error_code ec;
  fs::create_directories(c.data, ec);
  if (log_file_) std::fclose(log_file_);
  log_file_ = _wfopen((c.data / "launcher.log").c_str(), L"ab");
  {
    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    log(std::string("==== setup ") + when + " ====");
  }
  auto fail = [&](const std::string& why) {
    log("ERROR: " + why);
    std::lock_guard lk(mu_);
    view_.steps[step_].state = StepState::Failed;
    view_.error = why;
    view_.finished = true;
    view_.cancelled = cancel_.load();
  };
  auto notify = [&] {
    if (notify_) notify_();
  };
  std::wstring path = tool_path_prefix(c);
  // Runs a tool; ninja's "[n/m]" lines move the step's progress.
  std::string last_error;
  auto exec = [&](const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& cwd) -> bool {
    std::string line0 = narrow(exe.filename().wstring());
    for (const auto& a : args) line0 += " " + narrow(a);
    log("> " + line0);
    last_error.clear();
    RunResult r = runner_.run(exe, args, cwd, path, [&](const std::string& line) {
      log(line);
      if (line.size() > 3 && line[0] == '[') {
        unsigned a = 0, b = 0;
        if (std::sscanf(line.c_str(), "[%u/%u]", &a, &b) == 2 && b) set_fraction(double(a) / b);
      }
      if (line.rfind("error", 0) == 0 || line.find(": error:") != std::string::npos || line.rfind("FAILED", 0) == 0) last_error = line;
      notify();
    });
    if (r.cancelled || cancel_) return false;
    if (!r.started) {
      last_error = r.error;
      return false;
    }
    if (r.exit_code != 0) {
      if (last_error.empty()) last_error = line0.substr(0, line0.find(' ')) + " ended with code " + std::to_string(r.exit_code);
      return false;
    }
    return true;
  };
  auto finish_step = [&](size_t i, bool skipped) {
    set_step(i, skipped ? StepState::Skipped : StepState::Done);
    notify();
  };
  auto run_steps = [&]() -> bool {
    // Checks first: CPU, disk space.
    if (!x86_64_v3()) {
      step_ = kToolchain;
      fail("This CPU lacks AVX2/FMA3/BMI2 (x86-64-v3). The port needs an Intel Haswell (2013) or AMD Excavator/Zen CPU or newer.");
      return false;
    }
    unsigned long long need = space_needed(c), have = free_space(c.data);
    if (have && have < need) {
      step_ = kToolchain;
      fail(ac4::strprintf("Not enough disk space in %s: %.1f GB needed, %.1f GB free.", narrow(c.data.wstring()).c_str(),
                           double(need) / (1 << 30), double(have) / (1 << 30)));
      return false;
    }
    // 1. Toolchain.
    set_step(kToolchain, StepState::Running);
    notify();
    std::vector<Tool> tools = read_toolchain(c.src);
    if (tools.empty()) {
      fail("data\\toolchain.txt is missing from the port's folder.");
      return false;
    }
    bool fetched = false;
    size_t n_build = 0, i_build = 0;
    for (const Tool& t : tools) n_build += t.use == "build";
    for (const Tool& t : tools) {
      if (t.use != "build") continue;
      ++i_build;
      if (is_dir(tools_dir(c) / widen(t.check))) continue;
      fetched = true;
      fs::path zip = c.data / "downloads" / widen(t.url.substr(t.url.rfind('/') + 1));
      if (!is_file(zip) || content_hash(zip) != t.sha) {
        set_status("Downloading " + t.name);
        log("downloading " + t.url);
        notify();
        std::string err = download(widen(t.url), zip, [&](unsigned long long got, unsigned long long total) {
          if (!total) return;
          set_fraction((double(i_build - 1) + double(got) / double(total)) / double(n_build));
          set_status(ac4::strprintf("Downloading %s: %.0f / %.0f MB", t.name.c_str(), got / 1048576.0, total / 1048576.0));
          notify();
        }, cancel_);
        if (cancel_) return false;
        if (!err.empty()) {
          fail("Download of " + t.name + " failed: " + err);
          return false;
        }
        if (content_hash(zip) != t.sha) {
          fs::remove(zip, ec);
          fail("The downloaded " + t.name + " archive does not match its pinned SHA-256 (data\\toolchain.txt).");
          return false;
        }
      }
      set_status("Unpacking " + t.name);
      notify();
      fs::path into = t.into == "." ? tools_dir(c) : tools_dir(c) / widen(t.into);
      fs::create_directories(into, ec);
      wchar_t sys[MAX_PATH];
      GetSystemDirectoryW(sys, MAX_PATH);
      if (!exec(fs::path(sys) / "tar.exe", {L"-xf", w(zip), L"-C", w(into)}, c.data)) {
        fail("Unpacking " + t.name + " failed: " + last_error);
        return false;
      }
      fs::remove(zip, ec);  // the unpacked tools stay; the archive is not needed again
    }
    finish_step(kToolchain, !fetched);
    // 2. Preparation tools (configure: the generated code is picked up when it exists).
    auto configure = [&] {
      return exec(cmake_exe(c),
                  {L"-S", fwd(c.src), L"-B", fwd(build_dir(c)), L"-G", L"Ninja", L"-DCMAKE_BUILD_TYPE=Release", L"-DCMAKE_C_COMPILER=clang",
                   L"-DCMAKE_CXX_COMPILER=clang++", L"-DAC4_GEN_DIR=" + fwd(gen_dir(c) / "ppu"),
                   L"-DAC4_SPU_GEN_DIR=" + fwd(gen_dir(c) / "spu")},
                  c.data);
    };
    set_step(kTools, StepState::Running);
    notify();
    if (!configure() || !exec(cmake_exe(c), {L"--build", fwd(build_dir(c)), L"--target", L"ac4-prep", L"ac4-recomp", L"ac4-spurecomp"}, c.data)) {
      fail("Building the preparation tools failed: " + last_error);
      return false;
    }
    finish_step(kTools, false);
    fs::path prep = build_dir(c) / "ac4-prep.exe";
    // 3-7: the game files, each only when its inputs changed.
    struct Work {
      size_t step;
      std::function<bool()> run;
      const char* what;
    };
    std::vector<Work> work = {
        {kImport,
         [&] {
           // The player's decrypted files, verified by SHA-256 (ac4-prep reports any mismatch).
           Found f = find_files(c.disc, c.files);
           std::vector<std::wstring> a = {L"import",       L"--disc",        w(c.disc),
                                          L"--eboot",      w(f.eboot_elf),   L"--modules-dir",
                                          w(f.modules_dir), L"--flash-file", w(f.modules_dir / ki::kFlashAtrac.name),
                                          L"--out",        w(prepared(c)),   L"--flash-out",
                                          w(flash(c))};
           return exec(prep, a, c.data);
         },
         "Importing the game files failed"},
        {kRecompile,
         [&] {
           fs::remove_all(gen_dir(c) / "ppu", ec);
           std::vector<std::wstring> a = {L"emit", w(prepared(c) / "eboot.elf"), w(gen_dir(c) / "ppu")};
           for (const char* m : ki::kModules) a.insert(a.end(), {L"--module", w(prepared(c) / "lle" / (widen(m) + L".elf"))});
           a.insert(a.end(), {L"--hooks", w(c.src / "src" / "runtime" / "game" / "hooks.txt"), L"--patches",
                              w(c.src / "src" / "runtime" / "game" / "patches.txt")});
           return exec(build_dir(c) / "ac4-recomp.exe", a, c.data);
         },
         "Translating the game code failed"},
        {kSpu,
         [&] {
           fs::remove_all(gen_dir(c) / "spu", ec);
           fs::create_directories(gen_dir(c) / "spu", ec);
           fs::remove_all(c.data / "spu_cache", ec);  // records of another system software version go too
           return exec(prep, {L"spu-records", L"--manifest", w(c.src / "data" / "spu_manifest.txt"), L"--out", w(c.data / "spu_cache"), w(prepared(c))},
                       c.data) &&
                  exec(build_dir(c) / "ac4-spurecomp.exe", {w(gen_dir(c) / "spu"), w(c.data / "spu_cache")}, c.data);
         },
         "Translating the SPU code failed"},
    };
    for (Work& wk : work) {
      if (step_current(c, wk.step)) {
        finish_step(wk.step, true);
        continue;
      }
      set_step(wk.step, StepState::Running);
      notify();
      std::string s = stamp(c, wk.step);
      if (!wk.run()) {
        if (!cancel_) fail(std::string(wk.what) + ": " + last_error);
        return false;
      }
      write_state(c, wk.step, wk.step == kRecompile || wk.step == kSpu ? stamp(c, wk.step) : s);
      finish_step(wk.step, false);
    }
    // 8. The game.
    set_step(kBuild, StepState::Running);
    notify();
    if (!configure() || !exec(cmake_exe(c), {L"--build", fwd(build_dir(c)), L"--target", L"ac4"}, c.data)) {
      if (!cancel_) fail("Building the game failed: " + last_error);
      return false;
    }
    {
      std::ofstream(c.data / "state" / "built.txt", std::ios::binary) << stamp(c, kRecompile) + "\n" + stamp(c, kSpu);
    }
    finish_step(kBuild, false);
    // 9. Shaders known from the manifest, compiled before the first run.
    set_step(kShaders, StepState::Running);
    notify();
    if (!exec(game_exe(c),
              {L"--data", w(c.data), L"--disc", w(c.disc), L"--flash", w(flash_dir(c)), L"--warm-shaders", L"--shader-manifest",
               w(c.src / "data" / "shader_manifest.txt"), L"--keep-runs", L"10"},
              c.data)) {
      if (!cancel_) fail("Compiling the shaders failed: " + last_error);
      return false;
    }
    finish_step(kShaders, false);
    return true;
  };
  bool ok = run_steps();
  {
    std::lock_guard lk(mu_);
    view_.finished = true;
    view_.ok = ok;
    view_.cancelled = cancel_.load();
    if (ok) {
      view_.fraction = 1;
      view_.status = "Ready";
    } else if (cancel_) {
      view_.status = "Cancelled";
      view_.steps[step_].state = StepState::Failed;
    }
  }
  log(ok ? "setup finished" : cancel_ ? "setup cancelled" : "setup failed");
  if (log_file_) {
    std::fclose(log_file_);
    log_file_ = nullptr;
  }
  running_ = false;
  if (notify_) notify_();
}

}  // namespace launcher
