// ac4: runtime host for the recompiled Armored Core 4 (BLUS30027) executable.
//   ac4 [--disc <dir>] [--prepared <dir>] [--hdd0 <dir>] [--seconds N] [--trace-hle] [--log-level trace|debug|info]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#include "common/file.h"
#include "common/sfo.h"
#include "runtime/bindings.h"
#include "runtime/config.h"
#include "runtime/datawatch.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/window.h"
#include "runtime/rsx/rsx.h"
#include "runtime/sysutil.h"
#include "runtime/hle.h"
#include "runtime/input.h"
#include "runtime/loader.h"
#include "runtime/lv2/lv2.h"
#include "runtime/lv2/memory.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppu_runtime.h"
#include "runtime/ppu_thread.h"
#include "runtime/profiler.h"
#include "runtime/reservation.h"

namespace rt {
Config& cfg() {
  static Config c;
  return c;
}
void vdec_dump();
void lv2_init();  // lv2 subsystems that need setup after memory/loader
namespace lv2 {
void spu_dump();
void sync_dump();
}
void lwmutex_dump();
namespace gpu {
extern std::atomic<int> g_capture_requests;
}
}  // namespace rt

using namespace rt;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  Config& c = cfg();
  fs::path root = fs::current_path();
  c.disc_root = (root / "Armored Core 4 (USA)").string();  // the user's disc folder (read-only)
  c.prepared = (root / "local" / "game" / "BLUS30027").string();
  c.hdd0 = (root / "local" / "hdd0").string();
  c.hdd1 = (root / "local" / "hdd1").string();
  c.flash = (root / "local" / "game" / "firmware" / "dev_flash").string();
  c.spu_cache = (root / "local" / "spu_cache").string();
  c.shader_cache = (root / "local" / "shader_cache").string();
  c.input_config = (root / "local" / "config" / "input.ini").string();
  std::string level = "info";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--disc") c.disc_root = next();
    else if (a == "--prepared") c.prepared = next();
    else if (a == "--hdd0") c.hdd0 = next();
    else if (a == "--flash") c.flash = next();
    else if (a == "--seconds") c.seconds = std::atof(next().c_str());
    else if (a == "--trace-hle") c.trace_hle = true;
    else if (a == "--trace-lle") c.trace_lle = true;
    else if (a == "--log-level") level = next();
    else if (a == "--headless") c.headless = true;
    else if (a == "--lenient") c.lenient = true;
    else if (a == "--trace-spu" && i + 1 < argc) c.trace_spu = argv[++i];
    else if (a == "--trace-spu-limit" && i + 1 < argc) c.trace_spu_limit = unsigned(std::stoul(argv[++i]));
    else if (a == "--trace-spu-cats" && i + 1 < argc) c.trace_spu_cats = unsigned(std::stoul(argv[++i]));
    else if (a == "--profile") c.profile = next();
    else if (a == "--spu-collect") c.spu_collect = true;
    else if (a == "--spu-interp") c.spu_interp = true;
    else if (a == "--trace-rsx") c.trace_rsx = unsigned(std::stoul(next()));
    else if (a == "--trace-rsx-at") c.trace_rsx_at = std::atof(next().c_str());
    else if (a == "--dump-shaders") c.dump_shaders = true;
    else if (a == "--shot-surface") c.shot_surface = std::stol(next(), nullptr, 0);
    else if (a == "--watch-lines") c.watch_lines = next();
    else if (a == "--watch-access") c.watch_access = next();
    else if (a == "--dialog") c.dialog_script = next();
    else if (a == "--profile-at") c.profile_at = std::atof(next().c_str());
    else if (a == "--profile-split") c.profile_split = std::atof(next().c_str());
    else if (a == "--frame-report") c.frame_report = std::atof(next().c_str());
    else if (a == "--skip-fp") c.skip_fp = unsigned(std::stoul(next(), nullptr, 0));
    else if (a == "--input") c.input_script = next();
    else if (a == "--control") c.control_file = next();
    else if (a == "--input-config") c.input_config = next();
    else if (a == "--isolate-input") c.isolate_input = true;
    else if (a == "--gpu-submit") c.gpu_submit_thread = next() != "off";
    else if (a == "--priority") c.priority = next();
    else if (a == "--gpu-timing") c.gpu_timing = 1;
    else if (a == "--present-delay") c.present_delay_ms = std::atof(next().c_str());
    else if (a == "--fps-limit") {
      std::string v = next();
      if (!parse_fps_limit(v, &c.fps_limit)) {
        std::fprintf(stderr, "--fps-limit %s: use frames per second, console, display or unlimited\n", v.c_str());
        return 2;
      }
    }
    else if (a == "--vsync") c.vsync = next() != "off";
    else if (a == "--fullscreen") c.fullscreen = true;
    else if (a == "--test-overlay") c.test_overlay = next();
    else if (a == "--gpu-timing-draws") c.gpu_timing = 2;
    else if (a == "--spu-cache") c.spu_cache = next();
    else if (a == "--shot-interval") c.shot_interval = std::atof(next().c_str());
    else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  ac4::add_protected_root(c.disc_root);
  // Run directory: local/runs/<UTC timestamp>
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &now);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
  fs::path run = root / "local" / "runs" / stamp;
  fs::create_directories(run);
  c.run_dir = run.string();
  log_open((run / "ac4.log").string());
  log_set_level(level == "trace" ? Level::Trace : level == "debug" ? Level::Debug : Level::Info);
  hle::g_trace = c.trace_hle;
  for (size_t p = 0; p < c.watch_lines.size();) {  // --watch-lines ea:size,...
    size_t q = c.watch_lines.find(',', p);
    std::string item = c.watch_lines.substr(p, q == std::string::npos ? std::string::npos : q - p);
    size_t colon = item.find(':');
    if (colon != std::string::npos)
      res::watch_add(u32(std::stoul(item.substr(0, colon), nullptr, 0)), u32(std::stoul(item.substr(colon + 1), nullptr, 0)));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  prof::start(c.profile);
  LOG_I("main", "run dir %s", c.run_dir.c_str());
  if (!c.watch_access.empty() && !datawatch::configure(c.watch_access)) return 2;
  set_fatal_hook([] {
    prof::write_report(cfg().run_dir + "/profile.txt");
    datawatch::write_report(cfg().run_dir + "/access.txt");
  });
  bindings::load(c.input_config);
  // Optional process priority: with other programs keeping every core busy, each handoff between
  // the game's threads otherwise waits for a time slice (the frame rate collapses); above normal,
  // the game's threads run when they become ready. Off by default: it takes CPU time from other
  // programs while the game runs.
  if (!c.priority.empty()) {
    DWORD cls = c.priority == "high" ? HIGH_PRIORITY_CLASS : c.priority == "above-normal" ? ABOVE_NORMAL_PRIORITY_CLASS : 0;
    if (cls && SetPriorityClass(GetCurrentProcess(), cls))
      LOG_I("main", "process priority: %s", c.priority.c_str());
    else
      LOG_W("main", "--priority %s not applied (use above-normal or high)", c.priority.c_str());
  }
  if (c.isolate_input) LOG_I("input", "--isolate-input: the real keyboard, mouse and pads are ignored (scripted input only)");
  if (c.lenient) LOG_W("main", "SURVEY MODE (--lenient): unimplemented calls return 0; results are not evidence of working behaviour");
  LOG_I("main", "disc %s, prepared %s, hdd0 %s", c.disc_root.c_str(), c.prepared.c_str(), c.hdd0.c_str());
  try {
    auto sfo = ac4::sfo::parse(ac4::read_file(fs::path(c.disc_root) / "PS3_GAME" / "PARAM.SFO"));
    c.title_id = ac4::sfo::get_string(sfo, "TITLE_ID");
    c.title = ac4::sfo::get_string(sfo, "TITLE");
    LOG_I("main", "title %s (%s), version %s", c.title.c_str(), c.title_id.c_str(), ac4::sfo::get_string(sfo, "VERSION").c_str());
  } catch (const std::exception& e) {
    LOG_E("main", "cannot read the disc's PARAM.SFO: %s", e.what());
    return 1;
  }

  if (!vm::init()) return 1;
  install_fault_handler();
  ppu_runtime_init();
  ExecInfo ex;
  try {
    // Firmware modules relocated by ac4-prep (LLE) load first so the game binds to them.
    fs::path lle = fs::path(c.prepared) / "lle";
    if (fs::exists(lle))
      for (const auto& e : fs::directory_iterator(lle))
        if (e.path().extension() == ".elf") load_lle_module(ac4::read_file(e.path()));
    bind_lle_imports();
    ex = load_exec(ac4::read_file(fs::path(c.prepared) / "eboot.elf"));
    lv2::g_sdk_version = ex.sdk_version;
    lv2::memory_init(ex.sdk_version, u64(ex.segs_size) + ex.primary_stack);
    fs::path self = fs::path(c.disc_root) / "PS3_GAME" / "USRDIR" / "EBOOT.BIN";
    if (lv2::read_self_identity(self.string()))
      LOG_I("main", "SELF auth id 0x%016llx, control flags 0x%08x", (unsigned long long)lv2::g_auth_id, lv2::g_ctrl_flags1);
    else
      LOG_W("main", "cannot read the SELF header of %s; program authority ID unknown", self.string().c_str());
  } catch (const std::exception& e) {
    LOG_E("main", "load failed: %s", e.what());
    return 1;
  }
  lv2_init();
  if (!c.headless && !gpu::window_create(c.title + " - native port", 1280, 720, true))
    LOG_E("main", "window creation failed; running without presentation");
  else if (!c.headless && c.fullscreen)
    gpu::window_set_fullscreen(true);
  start_main_thread(ex.entry_opd, ex.primary_stack, ex.primary_prio, ex.tls_vaddr, ex.tls_filesz, ex.tls_memsz,
                    ex.malloc_pagesize, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
  auto t0 = std::chrono::steady_clock::now();
  double close_at = -1, next_shot = c.shot_interval > 0 ? c.shot_interval : -1, next_title = 1;
  static std::mutex stop_mu;  // the watchdog and the main loop may both decide to stop
  auto stop = [&](const char* why) {
    stop_mu.lock();  // never released: the process ends here
    LOG_I("main", "%s; stopping", why);
    LOG_I("main", "%s", rsx::stats().c_str());
    {
      u32 compiled = 0, cached = 0;
      double secs = 0;
      gpu::shader_stats(&compiled, &cached, &secs);
      LOG_I("main", "shaders: %u compiled (%.1f s), %u from the shader cache", compiled, secs, cached);
    }
    prof::write_report(c.run_dir + "/profile.txt");
    prof::cpu_report();
    thread_dump();
    lwmutex_dump();
    lv2::sync_dump();
    lv2::spu_dump();
    vdec_dump();
    res::watch_dump(c.run_dir + "/watch.txt");
    datawatch::write_report(c.run_dir + "/access.txt");
    LOG_I("dump", "PPU conditional stores refused by a change elsewhere in the 128-byte granule: %llu",
          (unsigned long long)ppu::granule_conflicts());
    rsx::dump();
    dump_hle_stats();
    log_flush();
    ExitProcess(0);
  };
  // Watchdog: the time limit and log flushing keep working while the main thread is busy in a
  // modal system dialog (or stuck), so bounded runs stay bounded and keep their logs.
  std::thread([&] {
    u64 last_flips = 0;
    double last_fps_at = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (now - last_fps_at >= 5) {  // displayed frame rate, for performance tracking
        u64 f = rsx::flips();
        std::string busy = prof::cpu_interval(4);
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        static u64 last_faults = 0;
        // Machine-wide CPU use next to ours: load from other programs shows up in measurements.
        auto ft = [](const FILETIME& t) { return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        FILETIME si, sk, su, pc, pe, pk, pu;
        GetSystemTimes(&si, &sk, &su);
        GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
        static u64 last_idle = 0, last_total = 0, last_ours = 0;
        u64 idle = ft(si), total = ft(sk) + ft(su), ours = ft(pk) + ft(pu);  // kernel time includes idle
        double sys_busy = total > last_total ? 100.0 * (1.0 - double(idle - last_idle) / double(total - last_total)) : 0;
        double our_share = total > last_total ? 100.0 * double(ours - last_ours) / double(total - last_total) : 0;
        if (last_fps_at > 0)
          LOG_I("perf", "%.1f frames/s; CPU: machine %.0f%%, this process %.0f%%; %.0f page faults/s, working set %llu MiB; "
                "busiest threads: %s",
                double(f - last_flips) / (now - last_fps_at), sys_busy, our_share,
                double(pmc.PageFaultCount - last_faults) / (now - last_fps_at), (unsigned long long)(pmc.WorkingSetSize >> 20),
                busy.c_str());
        last_faults = pmc.PageFaultCount;
        last_idle = idle;
        last_total = total;
        last_ours = ours;
        last_flips = f;
        last_fps_at = now;
      }
      log_flush();
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (c.seconds > 0 && el >= c.seconds + 2) stop("--seconds limit reached (watchdog)");
      if (input_quit_requested()) stop("quit requested in the control file");
    }
  }).detach();
  for (;;) {
    if (!c.headless) gpu::window_pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (gpu::window_close_requested() && close_at < 0) {
      LOG_I("main", "window closed: sending CELL_SYSUTIL_REQUEST_EXITGAME to the game");
      sysutil::post_event(0x0101, 0);
      close_at = el;
    }
    if (close_at >= 0 && el - close_at > 5.0) stop("game did not exit within 5 s of the exit request");
    if (next_shot > 0 && el >= next_shot) {
      ++gpu::g_capture_requests;
      next_shot += c.shot_interval;
    }
    if (el >= next_title) {
      next_title = el + 1;
      if (!c.headless) gpu::window_set_title(c.title + " - native port | " + rsx::stats());
    }
    if (c.seconds > 0 && el >= c.seconds) stop("--seconds limit reached");
  }
}
