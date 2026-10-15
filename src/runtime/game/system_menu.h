// The SYSTEM menu's PC SETTINGS row (system_menu.cpp; GAME_NOTES "SYSTEM menu layout").
#pragma once
#include <string>

#include "common/bytes.h"

namespace rt::game {

// The game's SYSTEM menu archive, relative to the disc root.
constexpr const char* kSystemMenuArchive = "PS3_GAME/USRDIR/bind/lang/us/menu/systemsetting.000";
// The ID of the added button (no AC4 menu uses it).
constexpr u32 kPcSettingsButton = 9050;

// The SYSTEM menu archive with the PC SETTINGS row, built once from the player's disc into
// <data_dir>\generated; its path, or "" (logged) when the disc's archive is not the known one or
// the copy cannot be made: the menu then stays as the game has it.
std::string system_menu_archive(const std::string& disc_root, const std::string& data_dir);

}  // namespace rt::game
