// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <sstream>

#include "ppu/ppu_recomp.h"
#include "runtime/bindings.h"
#include "runtime/datawatch.h"
#include "runtime/game/game_input.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/rsx/rsx.h"

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

// Inputs served by the axis accessor 0x70b610 (first come, first served; diagnostics).
struct AxisSlot {
  std::atomic<u32> input{0}, value{0}, forced{0};  // value / forced: float bits
  std::atomic<u64> reads{0};
  std::atomic<bool> overridden{false};
};
AxisSlot g_axes[48];

AxisSlot* axis_slot(u32 input) {
  for (AxisSlot& s : g_axes) {
    u32 cur = s.input.load(std::memory_order_relaxed);
    if (cur == input) return &s;
    if (cur == 0) {
      u32 expected = 0;
      if (s.input.compare_exchange_strong(expected, input) || expected == input) return &s;
    }
  }
  return nullptr;
}

float f32_of_bits(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
u32 bits_of_f32(float f) {
  u32 bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

// ---- Mouse look ----
// Each AC's operator writes its turn command into a virtual pad state with 0x70eb30(input, value)
// (the player's: the stick after the input layer's dead zone (~0.4) and curve, from 0x6eee98); the
// AC control reads it through 0x70b610 and turns at a rate proportional to it (measured: 0.1 ->
// 7.7, 0.25 -> 18.6, 0.5 -> 38.9, 1.0 -> 71 deg/s), and counter-steers rotation that does not come
// from that command. Mouse turning is therefore added to the player's command where the stick's is
// written: a value from raw mouse counts per frame time turns the AC by the same angle per count at
// any frame rate, within the AC's own turn rate limit. Input objects of every class (the player's
// seen with vtables 0x1228610 and 0x1228550, AI-driven ACs' 0x122e3c8) read their virtual pad
// state at +12 (vtable slot 2, 0x70b540); the player's is the one the operator writes.
constexpr u32 kAiInputVtable = 0x0122e3c8;
// The player operator (0x6eee98) sets the turn command through a temporary view object (vtable
// 0x1226c30, on the stack) from this call site; the setter sees no AI objects.
constexpr u32 kPlayerTurnSetSite = 0x006ef030;
// Sensitivity 1.0: 1000 counts per second turn the AC at its full rate (value 1.0), i.e. one count
// turns 1/1000 of the AC's full rate times one second (0.075 deg for an AC turning 75 deg/s).
constexpr double kTurnPerCount = 1.0 / 1000.0;
// Movement beyond the AC's turn rate is carried into the next frames, at most this many seconds
// of full-rate turning (no input lost to the limit, no long runaway turns after a flick).
constexpr double kMaxCarrySeconds = 0.25;
// The look pitch command (word 13 of the virtual pad state) is set by the player operator through
// the view's slot 9 (0x70b548, a thunk to the setter 0x70e960), from one of these call sites: the
// first passes the stick's value negated (the game's vertical inversion option), the second as is.
// The AC's look control (0x7a2fb8) reads it (0x70b7e8) and moves the look pitch (0x7a22d0, state
// at look control +48): the pitch velocity follows command x max speed through a first-order
// filter (60 steps per second), the angle is clamped to the AC's limits.
constexpr u32 kPlayerPitchSetSites[] = {0x006ef110, 0x006ef77c};
// Pitch per count relative to turning (same scale until the two rates are compared in degrees).
constexpr double kPitchPerCount = kTurnPerCount;
// Sign of the pitch command that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;

struct MouseAxis {
  float value = 0;     // this frame's command from the mouse
  double backlog = 0;  // counts beyond the AC's rate, still to apply
};
MouseAxis g_mouse_turn, g_mouse_pitch;
std::atomic<u32> g_turn_input{0};    // last player turn input seen (diagnostics)
std::atomic<u32> g_player_state{0};  // the player's virtual pad state (written through the view)
std::atomic<u32> g_look{0};          // the player AC's look control object (diagnostics)
std::atomic<int> g_trace_left{0};  // diagnostics: player turn reads / pad updates still to log
LARGE_INTEGER g_last_turn{};       // time of the last mouse look computation

// The player AC's world position (x, y, z, 1) lies at its look control - 0xD49C (found by memory
// search, runs 190713 / 191255; four ACs in the first mission run the look update, the AI-driven
// normals do not). Diagnostics only (axes command).
constexpr u32 kLookToPosition = 0xD49C;

bool is_player_input(u32 input) {
  u32 state = g_player_state.load(std::memory_order_relaxed);
  return state && input && rt::vm::committed(input, 16) && r32(input + 12) == state;
}

void mouse_axis(MouseAxis& a, s64 counts, double sens, double dt) {
  double c = a.backlog + double(counts);
  double value = std::clamp(c * sens / dt, -1.0, 1.0);
  double rest = c - value * dt / sens, max_rest = kMaxCarrySeconds / std::abs(sens);
  a.backlog = std::clamp(rest, -max_rest, max_rest);
  a.value = float(value);
}

// Once per game frame (the operator sets the player's turn command once per frame, before the
// pitch command): the mouse movement since the previous frame becomes this frame's turn and pitch
// values. The interval between these calls is the game's frame time; after a longer gap (menus,
// pause, a hitch) the movement gathered meanwhile is dropped instead of turning the AC all at once.
void mouse_look_frame() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_turn.QuadPart ? double(now.QuadPart - g_last_turn.QuadPart) / double(freq.QuadPart) : 0;
  g_last_turn = now;
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  const rt::bindings::MouseSettings& m = rt::bindings::mouse();
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if (dt <= 0 || dt > 0.25 || m.sensitivity <= 0) {
    if ((dx || dy || g_mouse_turn.backlog || g_mouse_pitch.backlog) && traced++ < 60)
      LOG_I("input", "mouse look: %lld, %lld counts dropped (%.0f ms since the previous frame)", (long long)dx,
            (long long)dy, dt * 1000);
    g_mouse_turn = g_mouse_pitch = MouseAxis{};
    return;
  }
  mouse_axis(g_mouse_turn, dx, m.sensitivity * kTurnPerCount, dt);
  double pitch_sens = m.sensitivity * kPitchPerCount * kPitchDownSign;
  mouse_axis(g_mouse_pitch, m.invert_y ? -dy : dy, pitch_sens, dt);
  // At a pitch limit the game discards motion beyond it; the carried mouse motion toward that limit
  // is dropped too, so pushing against the limit leaves no dead zone for the way back.
  if (u32 look = g_look.load(std::memory_order_relaxed); g_mouse_pitch.backlog != 0 && look && rt::vm::committed(look + 48, 40)) {
    float angle = f32_of_bits(r32(look + 48)), hi = f32_of_bits(r32(look + 48 + 32)), lo = f32_of_bits(r32(look + 48 + 36));
    double toward = g_mouse_pitch.backlog * pitch_sens;  // > 0: up
    if ((toward > 0 && angle >= hi) || (toward < 0 && angle <= lo)) g_mouse_pitch.backlog = 0;
  }
  if ((dx || dy || g_mouse_turn.backlog || g_mouse_pitch.backlog) && traced++ < 60)
    LOG_I("input", "mouse look: %lld, %lld counts + backlog -> turn %.3f, pitch %.3f over %.1f ms, backlog %.0f, %.0f",
          (long long)dx, (long long)dy, double(g_mouse_turn.value), double(g_mouse_pitch.value), dt * 1000,
          g_mouse_turn.backlog, g_mouse_pitch.backlog);
}

}  // namespace

// Input axis accessor (0x70b610: out = the input's axis value, clamped). Records each input's
// latest value; a forced value (axis control command) replaces what the game reads.
void hook_input_axis_get(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 out = u32(ctx.r[3]), input = u32(ctx.r[4]);
  orig(ctx);
  if (g_trace_left.load(std::memory_order_relaxed) > 0 && is_player_input(input) && g_trace_left.fetch_sub(1) > 0)
    LOG_I("input", "trace: turn read 0x%08x at 0x%08x, flip %llu, returned %.3f", input, u32(ctx.lr),
          (unsigned long long)rt::rsx::flips(), double(f32_of_bits(r32(out))));
  AxisSlot* s = axis_slot(input);
  if (!s) return;
  s->value.store(r32(out), std::memory_order_relaxed);
  s->reads.fetch_add(1, std::memory_order_relaxed);
  if (s->overridden.load(std::memory_order_relaxed)) w32(out, s->forced.load(std::memory_order_relaxed));
}

// Turn command setter (0x70eb30: r3 input object, f1 value). For the player's input object the
// mouse turn of this frame is added to the stick's value.
void hook_input_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 input = u32(ctx.r[3]);
  if (u32(ctx.lr) == kPlayerTurnSetSite) {
    static int logged = 0;
    if (g_turn_input.exchange(input) != input && logged++ < 4)
      LOG_I("input", "mouse look: player turn command set through 0x%08x (vtable %08x), pad state 0x%08x", input,
            r32(input), r32(input + 12));
    g_player_state.store(r32(input + 12), std::memory_order_relaxed);
    mouse_look_frame();
    float stick = float(ctx.f[1]);
    if (g_mouse_turn.value != 0) ctx.f[1] = double(std::clamp(stick + g_mouse_turn.value, -1.0f, 1.0f));
    if (g_trace_left.load(std::memory_order_relaxed) > 0 && g_trace_left.fetch_sub(1) > 0)
      LOG_I("input", "trace: turn set 0x%08x, stick %.3f, set %.3f", input, double(stick), ctx.f[1]);
  }
  orig(ctx);
}

// Look (pitch) command setter thunk (0x70b548: r3 view, f1 value). From the player operator, the
// mouse pitch of this frame is added to the stick's value (after the game's own inversion option).
void hook_input_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  if (lr == kPlayerPitchSetSites[0] || lr == kPlayerPitchSetSites[1]) {
    float stick = float(ctx.f[1]);
    if (g_mouse_pitch.value != 0) ctx.f[1] = double(std::clamp(stick + g_mouse_pitch.value, -1.0f, 1.0f));
    if (g_trace_left.load(std::memory_order_relaxed) > 0 && g_trace_left.fetch_sub(1) > 0)
      LOG_I("input", "trace: pitch set from 0x%08x, stick %.3f, set %.3f", lr, double(stick), ctx.f[1]);
  }
  orig(ctx);
}

// An AC's look control update (0x7a2fb8: r3 the look control, r4 its input, f1 the frame time).
// Records the player's for diagnostics (axes command): every input class reads its virtual pad
// state at +12 (vtable slot 2, 0x70b540), the player's is the one the operator writes.
void hook_look_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 look = u32(ctx.r[3]), input = u32(ctx.r[4]);
  if (is_player_input(input) && g_look.exchange(look) != look) {
    static int logged = 0;
    if (logged++ < 4) LOG_I("input", "player look control 0x%08x (input 0x%08x), pitch state at 0x%08x", look, input, look + 48);
  }
  orig(ctx);
}

// Pad object updates, once per frame and port: cellPadGetData(port) into the object, then the
// sticks normalized to floats in [-1, 1]. Two classes with the same layout at different offsets.
void hook_pad_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  publish(pad, 696, 1484);
}

void hook_pad_update_b(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  publish(pad, 372, 1160);
  if (r32(pad + 372) == 0 && g_trace_left.load(std::memory_order_relaxed) > 0 && g_trace_left.fetch_sub(1) > 0)
    LOG_I("input", "trace: pad update (port 0), flip %llu", (unsigned long long)rt::rsx::flips());
}

}  // namespace ppu

namespace rt::game {

void axes_dump() {
  if (u32 look = ppu::g_look.load(); look && vm::committed(look + 48, 40)) {  // the player's look pitch (0x7a22d0)
    u32 p = look + 48;
    auto f = [&](u32 off) { return double(ppu::f32_of_bits(ppu::r32(p + off))); };
    LOG_I("input", "look pitch 0x%08x: angle %g, velocity %g, mode %u, max speed %g, drag %g, limits %g .. %g", p, f(0), f(4),
          ppu::r32(p + 20), f(24), f(28), f(36), f(32));
  }
  if (u32 look = ppu::g_look.load(); look && vm::committed(look - ppu::kLookToPosition, 12)) {
    u32 pos = look - ppu::kLookToPosition;
    auto f = [](u32 a) { return double(ppu::f32_of_bits(ppu::r32(a))); };
    LOG_I("input", "player AC position 0x%08x: %.2f %.2f %.2f", pos, f(pos), f(pos + 4), f(pos + 8));
  }
  for (const auto& s : ppu::g_axes) {
    u32 input = s.input.load();
    if (!input) break;
    if (!vm::committed(input, 16)) continue;  // a stack temporary's frame may be gone
    u32 vt = ppu::r32(input);
    bool player = ppu::is_player_input(input);
    LOG_I("input", "axis input 0x%08x: %llu reads, value %g%s, %s (vtable %08x)", input, (unsigned long long)s.reads.load(),
          double(ppu::f32_of_bits(s.value.load())),
          s.overridden.load() ? ac4::strprintf(" (forced %g)", double(ppu::f32_of_bits(s.forced.load()))).c_str() : "",
          player ? "player" : vt == ppu::kAiInputVtable ? "ai" : "other", vt);
    if (!player) continue;
    for (u32 field : {8u, 12u}) {  // the virtual pad state copies the player's input refers to
      u32 obj = ppu::r32(input + field) & ~3u;
      if (!vm::committed(obj, 64)) continue;
      std::string w;
      for (u32 i = 0; i < 16; ++i) w += ac4::strprintf(" %08x", ppu::r32(obj + 4 * i));
      LOG_I("input", "  +%u -> 0x%08x:%s", field, obj, w.c_str());
    }
  }
}

void turn_trace(int n) { ppu::g_trace_left = n; }

bool axis_command(const std::string& args) {
  std::istringstream in(args);
  std::string a, v;
  in >> a >> v;
  if (a.empty() || v.empty()) return false;
  ppu::AxisSlot* s = ppu::axis_slot(u32(std::stoul(a, nullptr, 0)));
  if (!s) return false;
  if (v == "off") {
    s->overridden = false;
  } else {
    s->forced = ppu::bits_of_f32(std::stof(v));
    s->overridden = true;
  }
  LOG_I("input", "axis input %s: %s", a.c_str(), v == "off" ? "game value" : ("forced " + v).c_str());
  return true;
}

}  // namespace rt::game
