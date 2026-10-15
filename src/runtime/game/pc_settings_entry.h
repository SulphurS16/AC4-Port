// PC SETTINGS in the game's SYSTEM menu (GAME_NOTES "SYSTEM menu"). On the console, HDD INSTALL
// copies about 3.2 GB of the game's archives to the hard disk to shorten loading; the port reads
// the game files from the PC's own disk, so that entry is PC SETTINGS here: its label and help text
// are replaced in the game's loaded UI text (UTF-16BE strings in guest memory; the label keeps its
// length), and choosing it (the game starts its install task) opens the PC SETTINGS screen
// (ui/overlay.h), which answers the install question behind it with CANCEL when it closes.
#pragma once
#include <string>

namespace rt::game {

// A guest thread started (frame_policy.cpp passes every start on).
void pc_settings_thread_started(const std::string& name);

}  // namespace rt::game
