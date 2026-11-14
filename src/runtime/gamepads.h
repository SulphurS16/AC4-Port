// Game controllers for the pad emulation (sys_io), the system dialogs and PC SETTINGS: XInput
// controllers (Xbox and compatible) and PlayStation controllers (DUALSHOCK 4, DualSense, DualSense
// Edge; USB and Bluetooth) read through Windows' HID, all in XInput's layout so every reader treats
// them alike. Cross is A, Circle B, Square X, Triangle Y, L1 / R1 the shoulders, L2 / R2 the
// triggers (analog), Options START, Share / Create and the touchpad click BACK (the PS3's SELECT).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <cstddef>

#include "common/bytes.h"

namespace rt::pads {

constexpr int kSlots = 4;

// Controller slot `slot` (0-3): the XInput controllers first (in their XInput order), then the
// PlayStation controllers in the order they were connected. False when the slot holds none.
// Callers check input_isolated() themselves.
bool get(int slot, XINPUT_STATE* out);
// Vibration of the controller in `slot` (XInput's motor speeds; PlayStation controllers: none yet).
void vibrate(int slot, WORD left, WORD right);
// Every controller in use as one (the game's one player, pad port 0; the ACFA port's 0.7.6): the
// buttons of all, each trigger's and each stick's strongest. A controller counts once its state
// has changed since it was found, so a device that only ever reports one state (an idle virtual
// pad, a stuck trigger or stick) does not drive the game. False when no controller is connected.
// Before, each device had a port of its own: a DualSense behind another device (Steam Input's or
// DS4Windows' virtual pad, anything in XInput slot 0) drove the menus (the game takes any port
// there) but not the AC (port 0, run 20261114-100337), and a second device for the same
// controller moved the menus twice (players' reports, 2026-11-13).
bool get_merged(XINPUT_STATE* out);
// Two players on one PC (the game's MULTIPLAYER > SPLIT-SCREEN): the controllers in use one each,
// in the order of their first input (player 0 = pad port 0). False when there is no such player.
bool get_player(int player, XINPUT_STATE* out);

// One input report of a PlayStation controller (report id first) in XInput's layout; false when
// it is not a state report.
enum class Model { DualShock4, DualSense };
bool parse_report(Model model, bool bluetooth, const u8* report, size_t size, XINPUT_GAMEPAD* out);

}  // namespace rt::pads
