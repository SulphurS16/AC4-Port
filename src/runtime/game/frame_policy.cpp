// Frame rate policy for AC4 (GAME_NOTES "Frame pacing"): the game's simulation advances by
// measured time, so missions run at the configured target (--fps / settings.ini), but some menu
// fades count frames (twice as fast at 120 fps) and the system movie player fails above 60 Hz:
// outside missions the emulated vblank runs at up to 60 Hz. A mission runs while the game's fire
// control thread "FcsManager" exists (started at the sortie, exited when the mission ends).
#include "runtime/game/frame_policy.h"

#include <atomic>

#include "runtime/game/pc_settings_entry.h"
#include "runtime/gpu/display.h"

namespace rt::game {
namespace {
std::atomic<int> g_missions{0};
}

bool mission_running() { return g_missions.load(std::memory_order_relaxed) > 0; }

void thread_started(const std::string& name) {
  if (name == "FcsManager") {
    ++g_missions;
    gpu::display::gameplay_started();
  }
  pc_settings_thread_started(name);
}

void thread_exited(const std::string& name) {
  if (name == "FcsManager") {
    --g_missions;
    gpu::display::gameplay_stopped();
  }
}

}  // namespace rt::game
