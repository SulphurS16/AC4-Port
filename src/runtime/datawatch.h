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

// "SPEC,..." with SPEC = [BASE+]OFFSET[:LEN][:w][:v]. BASE names an address published by a hook
// (set_base; "pad" = the port 0 pad object); without it OFFSET is a guest address. LEN is 1, 2, 4
// or 8 (default 4); "w" watches writes only (default: reads and writes); "v" logs every hit with
// the value then (default: the first hit per accessing site). Returns false on errors.
bool configure(const std::string& specs);
// Live control ("watch SPEC" adds one, "watch clear" removes all); returns false on errors.
bool command(const std::string& args);
// Publishes a named base address; watches relative to it are (re)armed when it changes.
void set_base(const char* name, u32 addr);
void write_report(const std::string& path);

}  // namespace rt::datawatch
