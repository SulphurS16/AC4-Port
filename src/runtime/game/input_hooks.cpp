// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/bindings.h"
#include "runtime/datawatch.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"

namespace ppu {
namespace {

// Publishes the port 0 pad object ("pad") and its normalized stick floats ("sticks": right X,
// right Y, left X, left Y) as bases for --watch-access.
void publish(u32 pad, u32 port_offset, u32 sticks_offset) {
  if (r32(pad + port_offset) != 0) return;
  static std::atomic<u32> logged{0};
  if (logged.exchange(pad) != pad)
    LOG_I("input", "port 0 pad object 0x%08x, stick floats at 0x%08x (right X, right Y, left X, left Y)", pad,
          pad + sticks_offset);
  rt::datawatch::set_base("pad", pad);
  rt::datawatch::set_base("sticks", pad + sticks_offset);
}

// ---- Mouse look ----
// The player's operator (0xb52584) maps the pad through the input layer (0xb5006c: dead zone and
// curve; stick 0.569 -> 0.205 turn) and hands the AC's commands to its view object through
// virtual setters: slot 6 (0xb6b148) turn, slot 9 (0xb67970, a thunk to slot 8) look pitch, the
// latter negated first when the game's vertical inversion option is set. A setter stores the value
// only while the view's flags allow it (view +16 -> word & 2 for turn, & 4 for pitch). The game
// then smooths the stick: the AC's yaw rate and the look control's pitch velocity follow the
// command through drag.
//
// The mouse does not go that way: raw counts become angles (kRadPerCount per count times the
// sensitivity; no smoothing, acceleration or dead zone), and each frame the AC turns, and the view
// pitches, by the mouse's angle directly, at most at the speed of the stick fully pushed. Movement
// beyond that speed waits as a pending angle and follows at full speed in the next frames, so the
// AC turns until it points where the mouse points and no movement is lost. The pending angle is
// dropped only where the stick would be ignored too (the view's flags, menus and pauses, a gap of
// more than 0.25 s), during a view reset, and against a pitch limit (no dead zone on the way back).
//   Turn: an AC body turns by its yaw rate (body +484, radians per 1/60 s step), which the game
// updates once per frame from the frame's turn input (0xb656b8: turn object +28 through the drag
// +36). After that update the mouse's share of the frame is added to the rate, and taken off again
// before the next update, so the stick's own dynamics never see it. Full speed is what the stick
// fully pushed reaches: rate factor +112 times the AC's turn speed for the direction (its part data
// r32(+120) +284 / +288, chosen as 0xa10260 does) over the TOC constant at -16300, over the drag.
//   Pitch: the look control's pitch angle (+48) gets the mouse's step after its update; full speed
// max speed (+72) over drag (+76) per step; limits +80 (up) / +84 (down); mode +68 is 1 while a view
// reset recentres it.
constexpr u32 kPlayerTurnSetReturn = 0x00b52708;                          // operator: slot 6 call
constexpr u32 kPlayerPitchSetReturns[] = {0x00b52800, 0x00b52c30};       // slot 9: inverted / normal
constexpr double kRadPerCount = 3.14159265358979323846 / 2000;           // 0.09 degrees: 1000 counts turn 90
// Sign of the pitch that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;
// The AC's look control (0xa372e8: r3 control, r4 its input, f1 frame time) turns (0xa36c6c),
// then reads the pitch command (getter 0xb67cd4: the input's state +48) and integrates it
// (0xa36684, state at control + 48): angle +0 (radians, up positive), velocity +4, accumulator +8,
// mode +20 (1 while a view reset recentres), max speed +24, drag +28, limits +32 (upper) and +36
// (lower); the velocity is zeroed at a limit. Every input class reads its state through slot 2;
// the player's input object holds the state the player's view writes at +12.
constexpr u32 kPitchState = 48;
constexpr u32 kYawRate = 484;  // an AC body's yaw rate (body = turn object +4)

// The mouse's pending angles and this frame's facts (the game's main thread: the operator, the look
// control and the AC update all run there, in that order).
struct MouseLook {
  double turn = 0, pitch = 0;           // radians still to apply (turn: the stick's positive direction; pitch: up)
  bool turned = false, pitched = false;  // the mouse moved this frame (the tutorial's steps)
  int turn_dir = 0;                     // this frame's turn direction (sign)
};
MouseLook g_ml;
LARGE_INTEGER g_last_frame{};
std::atomic<u64> g_turn_calls{0}, g_pitch_calls{0};
std::atomic<int> g_look_trace{0};    // dev: frames of the player's look control still to log (turn_trace)
std::atomic<u32> g_heading_probe[4];  // dev: (sin, cos) pairs of heading matrices (turn_trace; up to four)
std::atomic<u32> g_player_state{0};  // the player's view state (view +12), written by the operator
std::atomic<u32> g_look{0};          // the player AC's look control
std::atomic<u32> g_player_turn{0};   // the player AC's turn object
float g_turn_added = 0;              // the yaw rate the mouse added this frame (taken off before the next update)
u32 g_turn_added_body = 0;
float g_turn_drag = 0;               // the last drag seen (+36 of a frame with turn input)

float f32_of_bits(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
float rf(u32 ea) { return f32_of_bits(r32(ea)); }
void wf(u32 ea, float v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  w32(ea, bits);
}

bool view_allows(u32 view, u32 mask) {
  u32 flags = view ? r32(view + 16) : 0;
  return flags && rt::vm::committed(flags, 4) && (r32(flags) & mask) != 0;
}

// Once per game frame, from the operator's turn call (before the pitch call, the look control and
// the AC's update): the mouse movement since the previous frame joins the pending angles.
void mouse_look_frame(u32 view) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) / double(freq.QuadPart) : 0;
  g_last_frame = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  const rt::bindings::MouseSettings& m = rt::bindings::mouse();
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if (dt <= 0 || dt > 0.25) {  // menus, a pause, a hitch: nothing gathered meanwhile turns the AC
    if ((dx || dy || g_ml.turn || g_ml.pitch) && traced++ < 60)
      LOG_I("input", "mouse look: %lld, %lld counts dropped (%.0f ms since the previous frame)", (long long)dx, (long long)dy, dt * 1000);
    g_ml = MouseLook{};
    return;
  }
  double t = double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;
  double p = double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;
  g_ml.turned = t != 0;
  g_ml.pitched = p != 0;
  g_ml.turn_dir = t > 0 ? 1 : t < 0 ? -1 : 0;
  g_ml.turn = view_allows(view, 2) ? g_ml.turn + t : 0;  // where the view takes no turn the stick turns nothing either
  g_ml.pitch = view_allows(view, 4) ? g_ml.pitch + p : 0;
  if ((dx || dy) && traced++ < 60)
    LOG_I("input", "mouse look: %lld, %lld counts over %.1f ms -> pending turn %.4f, pitch %.4f rad", (long long)dx, (long long)dy, dt * 1000,
          g_ml.turn, g_ml.pitch);
}

// The operator's turn call (slot 6) of the player: the player's view state, then the mouse.
void player_turn_call(Ctx& ctx) {
  g_turn_calls.fetch_add(1, std::memory_order_relaxed);
  u32 view = u32(ctx.r[3]);
  if (view && rt::vm::committed(view + 12, 4)) {
    u32 state = r32(view + 12);
    if (g_player_state.exchange(state) != state) {
      LOG_I("input", "mouse look: player view 0x%08x, state 0x%08x", view, state);
      rt::datawatch::set_base("state", state);  // --watch-access state+OFFSET (turn command +32, pitch +48)
    }
  }
  mouse_look_frame(view);
}

// The steady yaw rate of the stick fully pushed in a direction (radians per step), from the turn
// object's data as the game computes the stick's turn (0xb5a4f4 -> 0xa10260, 0xb64528); 0 if not
// known yet. toc: r2 of the AC update code (0xb656b8 and 0xb5a4f4 share it); 0xa10260 runs with
// r2 61892 lower (0x9a7700 adds it, 0xaf55a0 takes it off).
double full_turn_rate(u32 toc, u32 obj, int dir) {
  u32 part = r32(obj + 120), toc_a = toc - 61892;
  if (!part || !rt::vm::committed(part + 284, 8) || !rt::vm::committed(toc - 16300, 4) || !rt::vm::committed(toc_a - 15976, 52)) return 0;
  float c = rf(toc - 16300), c1 = rf(toc_a - 15956), c2 = rf(toc_a - 15976), c3 = rf(toc_a - 15928);
  static bool logged = false;
  if (!logged) {
    logged = true;
    LOG_I("input", "mouse look: turn speeds %g (+284) / %g (+288), factor %g, scale %g, full commands %g / %g, mean %g, drag %g", double(rf(part + 284)),
          double(rf(part + 288)), double(rf(obj + 112)), double(c), double(c1), double(c2), double(c3), double(rf(obj + 36)));
  }
  float cmd = dir > 0 ? 1.0f : -1.0f;
  float speed = cmd == c1 ? rf(part + 284) : cmd == c2 ? rf(part + 288) : (rf(part + 284) + rf(part + 288)) * c3;
  float drag = rf(obj + 36);
  if (drag > 0) g_turn_drag = drag;
  if (g_turn_drag <= 0 || c == 0) return 0;
  return std::fabs(double(rf(obj + 112)) * double(speed) / double(c) / double(g_turn_drag));
}

}  // namespace

// Pad object updates, once per frame and port: cellPadGetData(port, this + port offset + 8), then
// the sticks normalized to floats in [-1, 1] ((v / 255 - 0.5) * 2, Y inverted). Two classes with
// the same layout at different offsets (0x951f94: port +384, sticks +1172; 0x5ead28: port +736,
// sticks +1524).
void hook_pad_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  publish(pad, 384, 1172);
}

void hook_pad_update_b(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  publish(pad, 736, 1524);
}

// View slot 6 (0xb6b148: r3 view, f1 turn command): the stick's command goes on unchanged; from the
// player's operator, the frame's mouse movement is gathered (it turns the AC in ac_turn_update).
void hook_view_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) player_turn_call(ctx);
  orig(ctx);
}

// The tutorial's view class (vtable 0x10148768) has its own slot 6 (0xb6acf4): the same store, plus
// a flag its steps count ("turn 3 times") when the command is not zero. A mouse turn sets it with a
// command too small to turn the AC (1/10000 of the stick's), so the step counts the mouse too.
void hook_view_turn_set_b(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) {
    player_turn_call(ctx);
    if (g_ml.turned && ctx.f[1] == 0) ctx.f[1] = g_ml.turn_dir > 0 ? 1e-4 : -1e-4;
  }
  orig(ctx);
}

// View slot 9 (0xb67970, a thunk to slot 8: r3 view, f1 pitch command, r5 the operator's "pitch
// input" flag): the stick's command goes on unchanged. The tutorial view's slot 8 (0xb6ae5c) marks
// its "adjust view" steps on that flag: a mouse pitch raises it (slot 8 of the other views ignores it).
void hook_view_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  if (lr == kPlayerPitchSetReturns[0] || lr == kPlayerPitchSetReturns[1]) {
    g_pitch_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_ml.pitched) ctx.r[5] = 1;
  }
  orig(ctx);
}

// 0x9fc210 (every frame) and 0x9fc364 (while there is turn input): r3 an AC's controller, r4 its
// input. The player's (its input holds the player's view state) gives the player's turn object,
// r32(r32(r32(controller +4) +152) +260).
void note_turn_input(Ctx& ctx) {
  u32 ctl = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  if (!state || !input || !rt::vm::committed(input + 12, 4) || r32(input + 12) != state || !rt::vm::committed(ctl + 4, 4)) return;
  u32 a = r32(ctl + 4);
  if (!a || !rt::vm::committed(a + 152, 4)) return;
  u32 b = r32(a + 152);
  if (!b || !rt::vm::committed(b + 260, 4)) return;
  u32 obj = r32(b + 260);
  if (g_player_turn.exchange(obj) != obj) LOG_I("input", "mouse look: player turn object 0x%08x", obj);
}
void hook_ac_turn_input(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}
void hook_ac_turn_input_b(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}

// 0xb656b8 (r3 an AC's turn object): the AC body's yaw rate from the frame's turn input. For the
// player's: the mouse's share of the last frame comes off first (the game's update sees the stick's
// rate only), then this frame's share goes on: the pending angle, at most at full speed (with the
// stick, the two together stay within it).
void hook_ac_turn_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = u32(ctx.r[3]);
  if (!obj || obj != g_player_turn.load(std::memory_order_relaxed) || !rt::vm::committed(obj, 128)) {
    orig(ctx);
    return;
  }
  u32 body = r32(obj + 4);
  if (!body || !rt::vm::committed(body + kYawRate, 4)) {
    orig(ctx);
    return;
  }
  u32 toc = u32(ctx.r[2]);
  if (g_turn_added != 0 && g_turn_added_body == body) wf(body + kYawRate, rf(body + kYawRate) - g_turn_added);
  g_turn_added = 0;
  orig(ctx);
  double steps = double(rf(obj + 8)) * 60;  // the frame's time in 1/60 s steps
  if (g_ml.turn == 0 || steps <= 0 || steps > 15) return;
  double full = full_turn_rate(toc, obj, g_ml.turn > 0 ? 1 : -1);
  if (full <= 0) return;
  double rate = rf(body + kYawRate);
  double add = std::clamp(g_ml.turn / steps, std::min(0.0, -full - rate), std::max(0.0, full - rate));
  wf(body + kYawRate, float(rate + add));
  g_turn_added = float(add);
  g_turn_added_body = body;
  g_ml.turn -= add * steps;
  if (std::fabs(g_ml.turn) < 1e-7) g_ml.turn = 0;
}

namespace {
// The mouse's pitch step after the player's look control updated the angle from the stick: the
// pending angle, at most at full speed, within the limits (the rest toward a limit is dropped).
void mouse_pitch_step(u32 look, double dt) {
  u32 p = look + kPitchState;
  if (g_ml.pitch == 0 || !rt::vm::committed(p, 40)) return;
  if (r32(p + 20) == 1) {  // a view reset recentres the view: the mouse waits
    g_ml.pitch = 0;
    return;
  }
  double steps = dt * 60, maxs = rf(p + 24), drag = rf(p + 28), hi = rf(p + 32), lo = rf(p + 36);
  if (steps <= 0 || steps > 15 || maxs <= 0 || drag <= 0 || lo >= hi) return;
  double full = maxs / drag * steps;
  double step = std::clamp(g_ml.pitch, -full, full);
  double angle = rf(p), next = std::clamp(angle + step, lo, hi);
  wf(p, float(next));
  g_ml.pitch -= step;
  if ((g_ml.pitch > 0 && next >= hi) || (g_ml.pitch < 0 && next <= lo) || std::fabs(g_ml.pitch) < 1e-7) g_ml.pitch = 0;
}
}  // namespace

// The AC's look control (0xa372e8: r3 control, r4 input, f1 frame time). For the player's (its
// input holds the player's view state at +12): the mouse's pitch step after the update.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  bool player = state && input && rt::vm::committed(input + 12, 4) && r32(input + 12) == state;
  if (!player) {
    orig(ctx);
    return;
  }
  double dt = ctx.f[1];
  if (g_look.exchange(look) != look) {
    LOG_I("input", "mouse look: player look control 0x%08x (input 0x%08x), pitch state at 0x%08x", look, input, look + kPitchState);
    rt::datawatch::set_base("look", look);  // --watch-access look+OFFSET
  }
  // Diagnostics: the frame time the game hands its simulation (f1), summarized every 5 s.
  static double sum = 0, lo = 1e9, hi = 0;
  static u64 n = 0;
  static LARGE_INTEGER since{};
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (!since.QuadPart) since = now;
  sum += dt, lo = std::min(lo, dt), hi = std::max(hi, dt), ++n;
  if (double(now.QuadPart - since.QuadPart) / double(freq.QuadPart) >= 5.0) {
    LOG_I("input", "game frame time (player look control): %llu frames, mean %.2f ms, min %.2f, max %.2f", (unsigned long long)n,
          sum / double(n) * 1000, lo * 1000, hi * 1000);
    sum = 0, lo = 1e9, hi = 0, n = 0, since = now;
  }
  orig(ctx);
  mouse_pitch_step(look, dt);
}

}  // namespace ppu

