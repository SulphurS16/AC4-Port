// The game's live button assignment for the player (the operator's action table at operator +80;
// GAME_NOTES "Controls"): one input code per action, as the KEY TYPE (TYPE A / TYPE B / CUSTOM A /
// CUSTOM B, SYSTEM > CONTROL OPTIONS) and its KEY CONFIGURATION set it. Keyboard actions are
// translated through it in missions (bindings.cpp), so a key keeps its action under any key type.
#pragma once
#include "common/bytes.h"

namespace rt::game {

// Action slots of the table (operator +80; the same 19 slots and order as the ACFA port's table).
enum KeySlot : u32 {
  kSlotMoveForward = 0, kSlotMoveBack, kSlotStrafeRight, kSlotStrafeLeft, kSlotTurnRight, kSlotTurnLeft, kSlotLookUp, kSlotLookDown,
  kSlotResetView, kSlotLock, kSlotBoost, kSlotOverBoost, kSlotQuickBoost, kSlotRightWeapon, kSlotLeftWeapon, kSlotShoulder,
  kSlotSwitchRight, kSlotSwitchLeft, kSlotPause, kSlotCount
};
// Input codes in it: 00-03 left stick up / down / right / left, 04 / 05 right stick up / down,
// 06 / 07 right stick right / left, 08 Circle, 09 Triangle, 0a Square, 0b Cross, 0c L1, 0d L2,
// 0e R1, 0f R2, 10 L3, 11 R3, 12 SELECT, 13 START, 14-17 D-pad up / down / right / left; 0xfe: none.
// TYPE A: 00 01 02 03 06 07 04 05 11 10 0d 09 0f 0b 0a 08 0e 0c 13; TYPE B moves the weapons:
// use right / left R1 / L1, switch right / left Cross / Square (runs 20261029-140127).
constexpr u8 kCodeNone = 0xfe;

// Copies the latest assignment seen in gameplay (false before the first one). Thread-safe.
bool key_assignment(u8 codes[kSlotCount]);

}  // namespace rt::game
