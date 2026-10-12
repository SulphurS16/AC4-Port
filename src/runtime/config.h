// Runtime configuration (command line). Paths are host paths.
#pragma once
#include <string>

namespace rt {

struct Config {
  std::string disc_root;    // read-only PS3 disc folder (contains PS3_GAME)
  std::string title_id;     // from the disc's PARAM.SFO (TITLE_ID), e.g. BLUS30027
  std::string title;        // from the disc's PARAM.SFO (TITLE)
  std::string prepared;     // ac4-prep output (eboot.elf, update/)
  std::string hdd0;         // writable /dev_hdd0 (game data, saves)
  std::string hdd1;         // writable /dev_hdd1 (system cache)
  std::string flash;        // read-only /dev_flash (extracted firmware)
  std::string run_dir;      // this run's log/capture directory
  double seconds = 0;       // auto-exit after N seconds (0 = run until the game exits)
  bool trace_hle = false;
  bool trace_lle = false;           // log the game's calls into firmware modules run natively
  bool headless = false;
  double shot_interval = 0;  // seconds between automatic PNG captures (0 = off)
  bool lenient = false;      // survey mode: unimplemented calls return 0 (never for evidence)
  std::string trace_spu;     // SPU thread name to trace (MFC, mailboxes, events, stops)
  unsigned trace_spu_limit = 4000;  // maximum number of traced SPU operations
  unsigned trace_spu_cats = 7;      // 1 = MFC/atomics, 2 = channels, 4 = events/stops/group
  double profile_at = 0;            // start sampling at this time (seconds)
  double profile_split = 0;         // samples after this time go to profile_late.txt (0 = off)
  std::string profile;              // sample host threads whose name contains this (profile.txt)
  bool spu_collect = false;         // record SPU code without a recompiled version (training run)
  bool spu_interp = false;          // never use recompiled SPU code (diagnostics)
  unsigned trace_rsx = 0;           // log the first N RSX FIFO methods
  double trace_rsx_at = 0;          // ... executed from this many seconds after start
  bool dump_shaders = false;        // write generated HLSL to <run>/shaders
  long shot_surface = -1;           // also capture the local-memory surface at this offset
  unsigned skip_fp = 0;             // diagnostics: skip draws using this SET_SHADER_PROGRAM value
  double frame_report = -1;         // log one frame's clears/draws and dump its surfaces (seconds)
  std::string dialog_script;        // answers for system dialogs: "choose:N|choose:cancel|yes|no|ok,..."
  std::string watch_lines;          // "ea:size,..." ranges whose atomic operations are recorded
  std::string watch_access;         // hardware data watchpoints (datawatch.h), "[base+]off[:len][:w],..."
  std::string input_script;         // timed pad presses on port 0: "t:btn[+btn][:hold],..." (seconds)
  std::string control_file;         // live control: lines of --input items appended while running
  std::string input_config;         // keyboard/mouse bindings file (bindings.h)
  bool isolate_input = false;       // tests: ignore the real keyboard, mouse and pads (scripted only)
  bool gpu_submit_thread = true;    // D3D11 calls on their own thread (gpu/submit.h); off: FIFO thread
  std::string priority;             // process priority class: "" (normal), "above-normal", "high"
  int gpu_timing = 0;               // GPU time per pass (gpu/gpu_timing.h), logged every 5 s; 2: per draw
  std::string spu_cache;            // where collected SPU code records are written
  std::string shader_cache;         // compiled HLSL (derived from the game's shaders; machine-local)
};

Config& cfg();

}  // namespace rt
