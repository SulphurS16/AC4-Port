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

// One input report of a PlayStation controller (report id first) in XInput's layout; false when
// it is not a state report.
enum class Model { DualShock4, DualSense };
bool parse_report(Model model, bool bluetooth, const u8* report, size_t size, XINPUT_GAMEPAD* out);

}  // namespace rt::pads
