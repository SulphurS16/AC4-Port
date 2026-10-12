// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include "ppu/ppu_recomp.h"
#include "runtime/bindings.h"
#include "runtime/datawatch.h"
#include "runtime/game/game_input.h"
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
// only while the view's flags allow it (view +16 -> word & 2 for turn, & 4 for pitch). Mouse look is
// added to the player's value at those calls: raw counts per frame time become a command, so the
// AC turns by the same angle per count at any frame rate, within its own turn rate, and the
// game's limits, lock-on and scripts apply as they do to the stick.
constexpr u32 kPlayerTurnSetReturn = 0x00b52708;                          // operator: slot 6 call
constexpr u32 kPlayerPitchSetReturns[] = {0x00b52800, 0x00b52c30};       // slot 9: inverted / normal
// Sensitivity 1.0: 1000 counts per second give the full command (1.0), as in the ACFA port.
constexpr double kTurnPerCount = 1.0 / 1000.0;
constexpr double kPitchPerCount = kTurnPerCount;
// Movement beyond the AC's full rate is carried into the next frames, at most this many seconds of
// full-rate motion (no input lost to the limit, no long runaway turns after a flick).
constexpr double kMaxCarrySeconds = 0.25;
// Sign of the pitch command that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;
// The AC's look control (0xa372e8: r3 control, r4 its input, f1 frame time) turns (0xa36c6c),
// then reads the pitch command (getter 0xb67cd4: the input's state +48) and integrates it
// (0xa36684, state at control + 48): angle +0 (radians, up positive), velocity +4, accumulator +8,
// mode +20 (1 while a view reset recentres), max speed +24, drag +28, limits +32 (upper) and +36
// (lower); the velocity is zeroed at a limit. Every input class reads its state through slot 2;
// the player's input object holds the state the player's view writes at +12.
constexpr u32 kPitchState = 48;

struct MouseAxis {
  float value = 0;     // this frame's command from the mouse
  double backlog = 0;  // counts beyond the full rate, still to apply
};
MouseAxis g_mouse_turn, g_mouse_pitch;
LARGE_INTEGER g_last_frame{};
std::atomic<u64> g_turn_calls{0}, g_pitch_calls{0};
std::atomic<u32> g_player_state{0};  // the player's view state (view +12), written by the operator
std::atomic<u32> g_look{0};          // the player AC's look control

float f32_of_bits(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void mouse_axis(MouseAxis& a, s64 counts, double sens, double dt, double max_carry_s) {
  double c = a.backlog + double(counts);
  double value = std::clamp(c * sens / dt, -1.0, 1.0);
  double rest = c - value * dt / sens, max_rest = max_carry_s / std::abs(sens);
  a.backlog = std::clamp(rest, -max_rest, max_rest);
  a.value = float(value);
}

// Once per game frame (the operator sets the turn before the pitch): the mouse movement since the
// previous frame becomes this frame's turn and pitch commands. After a longer gap (menus, pause, a
// hitch) the movement gathered meanwhile is dropped instead of turning the AC all at once.
void mouse_look_frame() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) / double(freq.QuadPart) : 0;
  g_last_frame = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  const rt::bindings::MouseSettings& m = rt::bindings::mouse();
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if (dt <= 0 || dt > 0.25 || m.sensitivity <= 0) {
    if ((dx || dy || g_mouse_turn.backlog) && traced++ < 60)
      LOG_I("input", "mouse look: %lld, %lld counts dropped (%.0f ms since the previous frame)", (long long)dx, (long long)dy,
            dt * 1000);
    g_mouse_turn = g_mouse_pitch = MouseAxis{};
    return;
  }
  mouse_axis(g_mouse_turn, dx, m.sensitivity * kTurnPerCount, dt, kMaxCarrySeconds);
  double pitch_sens = m.sensitivity * kPitchPerCount * kPitchDownSign;
  mouse_axis(g_mouse_pitch, m.invert_y ? -dy : dy, pitch_sens, dt, kMaxCarrySeconds);
  // At a pitch limit the game discards motion beyond it; the carried mouse motion toward that limit
  // is dropped too, so pushing against the limit leaves no dead zone for the way back.
  if (u32 look = g_look.load(std::memory_order_relaxed);
      g_mouse_pitch.backlog != 0 && look && rt::vm::committed(look + kPitchState, 40)) {
    u32 p = look + kPitchState;
    float angle = f32_of_bits(r32(p)), hi = f32_of_bits(r32(p + 32)), lo = f32_of_bits(r32(p + 36));
    double toward = g_mouse_pitch.backlog * pitch_sens;  // > 0: up
    if ((toward > 0 && angle >= hi) || (toward < 0 && angle <= lo)) g_mouse_pitch.backlog = 0;
  }
  if ((dx || dy || g_mouse_turn.backlog || g_mouse_pitch.backlog) && traced++ < 60)
    LOG_I("input", "mouse look: %lld, %lld counts + backlog -> turn %.3f, pitch %.3f over %.1f ms, backlog %.0f, %.0f",
          (long long)dx, (long long)dy, double(g_mouse_turn.value), double(g_mouse_pitch.value), dt * 1000,
          g_mouse_turn.backlog, g_mouse_pitch.backlog);
}

bool view_allows(u32 view, u32 mask) {
  u32 flags = view ? r32(view + 16) : 0;
  return flags && rt::vm::committed(flags, 4) && (r32(flags) & mask) != 0;
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

// View slot 6 (0xb6b148: r3 view, f1 turn command). From the player's operator the mouse turn of
// this frame is added to the stick's command.
void hook_view_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) {
    g_turn_calls.fetch_add(1, std::memory_order_relaxed);
    if (u32 view = u32(ctx.r[3]); view && rt::vm::committed(view + 12, 4)) {
      u32 state = r32(view + 12);
      if (g_player_state.exchange(state) != state) LOG_I("input", "mouse look: player view 0x%08x, state 0x%08x", view, state);
    }
    mouse_look_frame();
    if (g_mouse_turn.value != 0 && view_allows(u32(ctx.r[3]), 2))
      ctx.f[1] = double(std::clamp(float(ctx.f[1]) + g_mouse_turn.value, -1.0f, 1.0f));
  }
  orig(ctx);
}

// View slot 9 (0xb67970, a thunk to slot 8: r3 view, f1 pitch command). From the player's operator
// the mouse pitch of this frame is added after the game's own inversion option.
void hook_view_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  if (lr == kPlayerPitchSetReturns[0] || lr == kPlayerPitchSetReturns[1]) {
    g_pitch_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_mouse_pitch.value != 0 && view_allows(u32(ctx.r[3]), 4))
      ctx.f[1] = double(std::clamp(float(ctx.f[1]) + g_mouse_pitch.value, -1.0f, 1.0f));
  }
  orig(ctx);
}

// The AC's look control (0xa372e8: r3 control, r4 input, f1 frame time). Records the player's,
// whose input holds the player's view state at +12, for the pitch limits and diagnostics.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  if (state && input && rt::vm::committed(input + 12, 4) && r32(input + 12) == state) {
    if (g_look.exchange(look) != look)
      LOG_I("input", "mouse look: player look control 0x%08x (input 0x%08x), pitch state at 0x%08x", look, input, look + kPitchState);
    // Diagnostics: the frame time the game hands its simulation (f1), summarized every 5 s.
    static double sum = 0, lo = 1e9, hi = 0;
    static u64 n = 0;
    static LARGE_INTEGER since{};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (!since.QuadPart) since = now;
    double dt = ctx.f[1];
    sum += dt, lo = std::min(lo, dt), hi = std::max(hi, dt), ++n;
    if (double(now.QuadPart - since.QuadPart) / double(freq.QuadPart) >= 5.0) {
      LOG_I("input", "game frame time (player look control): %llu frames, mean %.2f ms, min %.2f, max %.2f", (unsigned long long)n,
            sum / double(n) * 1000, lo * 1000, hi * 1000);
      sum = 0, lo = 1e9, hi = 0, n = 0, since = now;
    }
  }
  orig(ctx);
}

}  // namespace ppu

namespace rt::game {

void axes_dump() {
  LOG_I("input", "mouse look: %llu player turn commands, %llu pitch commands seen; state 0x%08x", (unsigned long long)ppu::g_turn_calls.load(),
        (unsigned long long)ppu::g_pitch_calls.load(), ppu::g_player_state.load());
  if (u32 state = ppu::g_player_state.load(); state && vm::committed(state, 64))
    LOG_I("input", "  view state: turn %g, pitch %g", double(ppu::f32_of_bits(ppu::r32(state + 32))),
          double(ppu::f32_of_bits(ppu::r32(state + 48))));
  if (u32 look = ppu::g_look.load(); look && vm::committed(look + ppu::kPitchState, 40)) {
    u32 p = look + ppu::kPitchState;
    auto f = [&](u32 off) { return double(ppu::f32_of_bits(ppu::r32(p + off))); };
    LOG_I("input", "  look pitch 0x%08x: angle %g, velocity %g, mode %u, max speed %g, drag %g, limits %g .. %g", p, f(0), f(4),
          ppu::r32(p + 20), f(24), f(28), f(36), f(32));
  }
}

void turn_trace(int) { LOG_W("input", "turntrace: not available for this title yet"); }

bool axis_command(const std::string&) {
  LOG_W("input", "axis: not available for this title yet");
  return true;
}

}  // namespace rt::game
