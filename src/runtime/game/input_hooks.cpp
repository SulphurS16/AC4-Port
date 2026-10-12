// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
//
// Armored Core 4's turn/look command path has not been located yet, so mouse look is not wired
// into the game: the mouse drives only what the bindings file maps to pad buttons. The ACFA port
// (same engine lineage) added raw mouse counts, scaled by frame time, to the player's turn and
// pitch commands where the stick's values are written (after the stick's dead zone and curve), so
// that the game's own limits, collision, lock-on and scripts apply; the same approach is planned
// here once the AC4 functions are found.
#include <string>

#include "runtime/game/game_input.h"
#include "runtime/log.h"

namespace rt::game {

void axes_dump() { LOG_W("input", "axes: the game's input axes are not located for this title yet"); }

void turn_trace(int) { LOG_W("input", "turntrace: the game's turn command is not located for this title yet"); }

bool axis_command(const std::string&) {
  LOG_W("input", "axis: the game's input axes are not located for this title yet");
  return true;
}

}  // namespace rt::game
