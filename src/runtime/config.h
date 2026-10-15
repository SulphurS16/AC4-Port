// Runtime configuration (command line and the shared settings file). Paths are host paths.
#pragma once
#include <atomic>
#include <cstdlib>
#include <string>

namespace rt {

struct Config {
  std::string data_dir;     // the installation's data folder (prepared data, saves, caches, settings, runs)
  std::string settings_file;  // shared settings (common/settings_file.h): <data>\config\settings.ini
  unsigned keep_runs = 0;   // run directories kept under <data>\runs (0: all)
  std::string disc_root;    // read-only PS3 disc folder (contains PS3_GAME)
  std::string title_id;     // from the disc's PARAM.SFO (TITLE_ID), e.g. BLUS30027
  std::string title;        // from the disc's PARAM.SFO (TITLE)
  std::string prepared;     // ac4-prep output (eboot.elf, update/)
  std::string hdd0;         // writable /dev_hdd0 (game data, saves)
  std::string saves;        // the player's save folder (--user DIR: DIR\savedata), else hdd0's own
  std::string hdd1;         // writable /dev_hdd1 (system cache)
  std::string flash;        // read-only /dev_flash (extracted firmware)
  std::string run_dir;      // this run's log/capture directory
  bool warm_shaders = false;        // compile the shader manifest into the shader cache, then exit
  std::string priority;             // process priority class: "" (normal), "above-normal", "high"
  std::string pacing = "auto";      // emulated vblank source (gpu/display.h): auto | display | clock
  int render_scale = 0;             // percent; 0 = the settings file's (development override --render-scale)
  std::atomic<double> fps{60};      // game frame rate target (--fps N | display: 0 = the display's refresh | uncapped: -1); PC SETTINGS changes it
  bool fullscreen = false;          // start in borderless fullscreen (F11 / Alt+Enter toggle)
  bool windowed = false;            // --windowed: a window whatever the settings say
  int window_w = 1280, window_h = 720;  // window client size (windowed)
  std::string spu_cache;            // where collected SPU code records are written
  std::string shader_cache;         // compiled HLSL (derived from the game's shaders; machine-local)
  // Player builds: the developer options are fixed, so the code they gate is compiled out.
  static constexpr double seconds = 0;
  static constexpr bool trace_hle = false, trace_lle = false, headless = false, lenient = false;
  static constexpr double shot_interval = 0, profile_at = 0, profile_split = 0, trace_rsx_at = 0, frame_report = -1;
  static constexpr bool spu_collect = false, shader_collect = false, spu_interp = false, dump_shaders = false;
  static constexpr unsigned trace_rsx = 0, skip_fp = 0, trace_spu_limit = 0, trace_spu_cats = 0;
  static constexpr long shot_surface = -1;
  static constexpr bool isolate_input = false, gpu_submit_thread = true, audio_capture = false, gcm_wait_spin = false;
  static constexpr bool verify_native = false, texture_bc = true;
  static constexpr int gpu_timing = 0;
  static constexpr unsigned long long affinity = 0;
  static constexpr double present_delay_ms = 0;
  static inline const std::string shader_records{}, test_overlay{};
};

Config& cfg();

}  // namespace rt
