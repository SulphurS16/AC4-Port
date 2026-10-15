// Hardware data watchpoints for reverse engineering (--watch-access): which recompiled guest
// functions read or write given guest addresses. Uses the x86-64 debug registers (at most four
// watches of 1, 2, 4 or 8 aligned bytes) on every registered host thread (threads started later
// are armed within 0.2 s). Each accessing function, with its first two host callers, is logged
// once when first seen and counted; the counts go to access.txt in the run directory.
// Diagnostics only: every access traps, so watching hot data slows the game.
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt::datawatch {

// Player builds: no watchpoints.
inline void set_base(const char*, u32) {}
}  // namespace rt::datawatch
