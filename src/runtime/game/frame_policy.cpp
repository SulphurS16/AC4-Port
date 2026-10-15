// Frame rate policy for AC4 (GAME_NOTES "Frame pacing"): the game's simulation advances by
// measured time, so gameplay runs at the configured target (--fps / settings.ini), but some menu
// fades count frames (twice as fast at 120 fps) and the system movie player fails above 60 Hz:
// outside gameplay the emulated vblank runs at up to 60 Hz. Gameplay is the AC's fire control
// thread "FcsManager" alive inside a sortie scene (threads named *SortieScene: Ac4SortieScene for
// missions; the tutorial's, AC TEST's and the arena's own): the hub runs an FcsManager of its own,
// and a sortie scene starts already at the briefing (run 20261015-144257: hub FcsManager 42-54 s,
// Ac4SortieScene from 54 s, the mission's FcsManager 86-172 s).
#include "runtime/game/frame_policy.h"

#include <atomic>
#include <mutex>

#include "runtime/game/pc_settings_entry.h"
#include "runtime/gpu/display.h"

namespace rt::game {
namespace {
std::mutex g_mu;
int g_fcs = 0, g_sorties = 0;
std::atomic<bool> g_gameplay{false};

bool is_sortie(const std::string& name) {
  constexpr const char* kSuffix = "SortieScene";
  return name.size() >= 11 && name.compare(name.size() - 11, 11, kSuffix) == 0;
}

// Called with g_mu held after a count changed.
void update() {
  bool now = g_fcs > 0 && g_sorties > 0;
  if (now == g_gameplay.load(std::memory_order_relaxed)) return;
  g_gameplay = now;
  if (now) gpu::display::gameplay_started();
  else gpu::display::gameplay_stopped();
}
}  // namespace

bool mission_running() { return g_gameplay.load(std::memory_order_relaxed); }

void thread_started(const std::string& name) {
  if (name == "FcsManager" || is_sortie(name)) {
    std::lock_guard lk(g_mu);
    (name == "FcsManager" ? g_fcs : g_sorties)++;
    update();
  }
  pc_settings_thread_started(name);
}

void thread_exited(const std::string& name) {
  if (name == "FcsManager" || is_sortie(name)) {
    std::lock_guard lk(g_mu);
    int& n = name == "FcsManager" ? g_fcs : g_sorties;
    if (n > 0) --n;
    update();
  }
}

}  // namespace rt::game
