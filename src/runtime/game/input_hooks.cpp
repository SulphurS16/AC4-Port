// Hooks on the game's input code (listed in hooks.txt; facts in docs/GAME_NOTES.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "ppu/ppu_recomp.h"
#include "runtime/bindings.h"
#include "runtime/datawatch.h"
#include "runtime/game/key_assign.h"
#include "runtime/game/menu_mouse.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/memory.h"

void f_00b5a4f4(ppu::Ctx&);  // an AC's turn step (r3 turn object, f1 command): the full-speed probe
void f_00af5ac0(ppu::Ctx&);  // the game parameters' singleton (a stub that derives its TOC from the caller's)

namespace ppu {
namespace {

// ---- The player ----
// The player's operator (0xb52584) maps the pad through the input layer (0xb5006c: dead zone and
// curve; stick 0.569 -> 0.205 turn) and hands the AC's commands to an input object through virtual
// setters: slot 6 turn, slot 9 look pitch (a thunk to slot 8), the latter negated first when the
// game's vertical inversion option is set. In missions and AC TEST the object is the player's view
// (vtable 0x101486b0: turn 0xb6b148, pitch 0xb67970 -> 0xb6b2a4; a setter stores the value only
// while the view's flags allow it, view +16 -> word & 2 for turn, & 4 for pitch), in the tutorial
// its own view class (0x10148768: 0xb6acf4, 0xb6ae5c, marking the input its steps count), in the
// SIMULATOR (VsSortieScene, the VS modes' scene) the plain input class 0x1013f400 (turn 0xb67cc8,
// pitch 0x9a9e1c -> 0xb67d94: plain stores; runs 20261029-134636 / -135126). All are called from the
// same operator sites (turn: returns to 0xb52708), so the call site identifies the player in every
// mode; its r31 is then the operator's action table (key_assign.h).
constexpr u32 kPlayerTurnSetReturn = 0x00b52708;                     // operator: slot 6 call
constexpr u32 kPlayerPitchSetReturns[] = {0x00b52800, 0x00b52c30};  // slot 9: inverted / normal
constexpr u32 kInputToc = 0x00f04e58;  // the TOC of the operator code (OPDs of 0xb5006c and the turn step 0xb5a4f4)
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadPerCount = kPi / 2000;  // 0.09 degrees: 1000 counts turn 90
// Sign of the pitch that looks down (mouse moved toward the user: positive raw Y counts).
constexpr double kPitchDownSign = -1.0;
// The AC's look control (0xa372e8: r3 control, r4 its input, f1 frame time) turns (0xa36c6c: the
// yaw block at +4), then reads the pitch command (getter 0xb67cd4: the input's state +48) and
// integrates it (0xa36684, state at control + 48): angle +0 (radians, up positive), velocity +4,
// accumulator +8, mode +20 (1 while a view reset recentres), max speed +24, drag +28, limits +32
// (upper) and +36 (lower); the velocity is zeroed at a limit. Every input class reads its state
// through slot 2; the player's input object holds the state the operator writes at +12.
constexpr u32 kLookYaw = 4;
constexpr u32 kPitchState = 48;
constexpr u32 kYawRate = 484;       // an AC body's yaw rate (radians per 1/60 s step; body = turn object +4)
constexpr u32 kBodyForward = 432;   // an AC body's rotation row 2: x +432, z +440; heading atan2(x, z), right turns add
// The tutorial's steps count an input that lasts 0.5 s without a gap (its monitor 0xb69658: per
// input a hold time of 0.5 s, a release time of 1/12 s). A mouse movement counts as such an input
// from its start until kTutorialHold after its end (pitch: the operator's "pitch input" flag; turn:
// a command of kTutorialTurn when the AC is not turning already, too small to turn it).
constexpr double kTutorialHold = 0.6;
constexpr double kTutorialTurn = 1e-4;

std::atomic<u32> g_player_state{0};  // the player's input state (input object +12), written by the operator
std::atomic<u32> g_look{0};          // the player AC's look control
std::atomic<u32> g_player_turn{0};   // the player AC's turn object
std::atomic<u64> g_turn_calls{0}, g_pitch_calls{0};

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

double wrap(double a) { return std::remainder(a, 2 * kPi); }  // into [-pi, pi]
double now_seconds() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return double(now.QuadPart) / double(freq.QuadPart);
}

bool view_allows(u32 view, u32 mask) {
  u32 vt = view ? r32(view) : 0;
  if (vt != 0x101486b0 && vt != 0x10148768) return true;  // the plain input class stores every command
  u32 flags = r32(view + 16);
  return flags && rt::vm::committed(flags, 4) && (r32(flags) & mask) != 0;
}

// The heading of an AC body (radians; turning right adds), from its rotation.
bool heading_of(u32 body, double* h) {
  if (!body || !rt::vm::committed(body + kBodyForward, 12)) return false;
  double x = rf(body + kBodyForward), z = rf(body + kBodyForward + 8);
  if (!(x * x + z * z > 0.25)) return false;  // not a rotation (and not NaN)
  *h = std::atan2(x, z);
  return true;
}
u32 player_body() {
  u32 turn = g_player_turn.load(std::memory_order_relaxed);
  return turn && rt::vm::committed(turn + 4, 4) ? r32(turn + 4) : 0;
}

// The mouse's recent movement for the tutorial's steps (both mouse modes).
struct MouseActivity {
  double turned_at = -1e9, pitched_at = -1e9;  // the mouse's last movement in each axis (seconds)
  int turn_dir = 0;                            // the direction of the last mouse turn
  bool turning() const { return now_seconds() - turned_at < kTutorialHold; }
  bool pitching() const { return now_seconds() - pitched_at < kTutorialHold; }
};
MouseActivity g_activity;

// ---- The game's button assignment (key_assign.h) ----
// The operator's action table, at its turn call in r31 (operator +80): kept for the keyboard's
// translation (bindings.cpp) and the mouse's stick axes.
std::mutex g_keys_mu;
u8 g_keys[rt::game::kSlotCount];
bool g_keys_known = false;

void note_key_table(Ctx& ctx) {
  u32 table = u32(ctx.r[31]);
  if (!table || !rt::vm::committed(table, rt::game::kSlotCount)) return;
  u8 codes[rt::game::kSlotCount];
  for (u32 i = 0; i < rt::game::kSlotCount; ++i) {
    codes[i] = r8(table + i);
    if (codes[i] > 0x17 && codes[i] < 0xfe) return;  // not an action table
  }
  std::lock_guard lk(g_keys_mu);
  if (!g_keys_known || std::memcmp(codes, g_keys, sizeof codes) != 0) {
    std::string s;
    for (u8 c : codes) s += ac4::strprintf(" %02x", c);
    LOG_I("input", "the game's button assignment (action table 0x%08x):%s", table, s.c_str());
  }
  std::memcpy(g_keys, codes, sizeof codes);
  g_keys_known = true;
}
bool keys(u8 codes[rt::game::kSlotCount]) {
  std::lock_guard lk(g_keys_mu);
  if (!g_keys_known) return false;
  std::memcpy(codes, g_keys, rt::game::kSlotCount);
  return true;
}

// ---- The stick's response (mouse mode "stick") ----
// The stick reaches the operator through two maps of each axis: the engine's input layer takes
// kInputLayerDeadZone off and stretches the rest to the full range ((d - 0.36) / 0.64), then
// 0xb5006c a dead zone and two straight pieces given by the game's parameters (0xaf5ac0 -> +4 ->
// slot 7: +688 the dead zone's edge, +692 the value giving 0.5, +696 the one giving 1.0; 0.1, 0.65,
// 0.99 here, which reproduce the measured 0.569 -> 0.205). The mouse's deflection is the inverse of
// both, so the command is linear in the mouse's speed (the ACFA port's mouse stick, which has the
// same input path at other addresses).
constexpr u32 kParamStickCurve = 688;  // game parameters: the stick's dead zone edge, half and full deflection
constexpr u32 kOptionsInvertY = 52;    // the options block (r32(TOC - 17924)): vertical inversion
constexpr double kInputLayerDeadZone = 0.36;
constexpr double kNearFull = 0.995;    // a command just short of full (the mean turn speed still applies)

struct StickCurve {
  double dead = 0, half = 0, full = 0;  // input layer values that give the commands 0, 0.5 and 1 (0xb5006c)
  bool known = false;
  bool shaped() const { return dead < half && half < full; }
  double value(double d) const {  // the command for a stick deflection (signed)
    double a = std::max(0.0, (std::fabs(d) - kInputLayerDeadZone) / (1 - kInputLayerDeadZone)), v;
    if (!shaped()) v = std::min(a, 1.0);
    else if (a <= dead) v = 0;
    else if (a <= half) v = (a - dead) / (half - dead) * 0.5;
    else if (a <= full) v = 0.5 + (a - half) / (full - half) * 0.5;
    else v = 1.0;
    return d < 0 ? -v : v;
  }
  double deflection(double c) const {  // the stick deflection for a command (signed; the inverse of value)
    double a = std::min(std::fabs(c), 1.0), d;
    if (a <= 0) return 0;
    if (!shaped()) d = a;
    else if (a <= 0.5) d = dead + a / 0.5 * (half - dead);
    else d = half + (a - 0.5) / 0.5 * (full - half);
    d = std::min(kInputLayerDeadZone + d * (1 - kInputLayerDeadZone), 1.0);
    return c < 0 ? -d : d;
  }
};
StickCurve g_curve;  // main thread
bool g_look_inverted = false;

// Once per frame from the player's operator: the stick curve from the game's parameters (the call
// chain 0xb5006c makes) and the vertical inversion option.
void read_stick_settings(Ctx& ctx) {
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x400) & ~15u;
  w64(sp, ctx.r[1]);  // back chain
  ctx.r[1] = sp;
  ctx.r[2] = kInputToc;
  f_00af5ac0(ctx);
  u32 params = 0, single = u32(ctx.r[3]);
  if (single && rt::vm::committed(single + 4, 4)) {
    u32 obj = r32(single + 4), vt = obj && rt::vm::committed(obj, 4) ? r32(obj) : 0;
    u32 opd = vt && rt::vm::committed(vt + 28, 4) ? r32(vt + 28) : 0;
    if (opd && rt::vm::committed(opd, 8)) {
      ctx.r[3] = obj;
      ctx.r[2] = r32(opd + 4);
      icall(ctx, r32(opd));
      params = u32(ctx.r[3]);
    }
  }
  ctx = saved;
  if (params && rt::vm::committed(params + kParamStickCurve, 12)) {
    StickCurve c;
    c.dead = rf(params + kParamStickCurve);
    c.half = rf(params + kParamStickCurve + 4);
    c.full = rf(params + kParamStickCurve + 8);
    c.known = true;
    if (!g_curve.known || c.dead != g_curve.dead || c.half != g_curve.half || c.full != g_curve.full)
      LOG_I("input", "stick response (game parameters 0x%08x): dead zone to %.4f, half command at %.4f, full at %.4f%s", params, c.dead, c.half,
            c.full, c.shaped() ? "" : " (not shaped: the deflection as it is)");
    g_curve = c;
  }
  u32 options = r32(kInputToc - 17924);
  g_look_inverted = options && rt::vm::committed(options + kOptionsInvertY, 4) && r32(options + kOptionsInvertY) != 0;
}

// The steady yaw rate of the stick fully pushed in a direction (radians per step): the game's own
// turn step (0xb5a4f4) run as a probe with a full command on the player's turn object, its
// accumulator (+28) and drag (+36) restored after it. The step adds +112 x S / C(TOC -16300) x
// command to +28 (S: the AC's turn speed for that direction, 0xa10260: +284 for +1, +288 for -1,
// their mean for any other command), and the yaw integrator (0xb656b8 -> 0xb64528) settles where
// the drag takes it back off: rate = addition / drag (GAME_NOTES "Turn physics"). 0 if not known.
double g_full[2] = {0, 0};  // the stick's full turn rate for -1 / +1 (radians per step; 0: not known)
double full_turn_rate(Ctx& ctx, u32 obj, int dir) {
  if (!rt::vm::committed(obj, 192)) return 0;
  u32 acc = r32(obj + 28), drag = r32(obj + 36);
  float before = rf(obj + 28);
  Ctx saved = ctx;
  u32 sp = (u32(ctx.r[1]) - 0x400) & ~15u;
  w64(sp, ctx.r[1]);  // back chain
  ctx.r[1] = sp;
  ctx.r[2] = kInputToc;
  ctx.r[3] = obj;
  ctx.f[1] = dir > 0 ? 1.0 : -1.0;
  f_00b5a4f4(ctx);
  float add = rf(obj + 28) - before, turn_drag = rf(obj + 36);
  w32(obj + 28, acc);
  w32(obj + 36, drag);
  ctx = saved;
  static int logged = 0;
  double full = turn_drag > 0 ? std::fabs(double(add)) / double(turn_drag) : 0;
  if (logged++ < 4) LOG_I("input", "mouse: full stick turn step %g, drag %g -> %g rad per step (%s)", double(add), double(turn_drag), full, dir > 0 ? "right" : "left");
  return full;
}

double pitch_full_rate() {  // the look's steady pitch rate at a full command (radians per step; 0: not known)
  u32 look = g_look.load(std::memory_order_relaxed);
  if (!look || !rt::vm::committed(look + kPitchState + 24, 8)) return 0;
  double speed = rf(look + kPitchState + 24), drag = rf(look + kPitchState + 28);
  return speed > 0 && drag > 0 ? speed / drag : 0;
}

// ---- The mouse as the right stick (mouse mode "stick", the default since 0.6.0) ----
// The mouse deflects the controller's right stick where the game reads it: the port 0 pad object's
// normalized stick floats after each update (0x951f94: right X +1172, right Y +1176, left X +1180,
// left Y +1184, up positive), so everything the game does with the stick follows in every mode
// (missions, AC TEST, the tutorial, the SIMULATOR): the AC's turn and the look up and down with the
// game's own smoothing, the view, the FCS. A frame's mouse movement asks for an angle (kRadPerCount
// per count times the sensitivity); the command is the one whose steady rate covers that angle over
// the frame: the angle over the full rate times the frame's 1/60 s steps (turn: the turn step's
// probe, full_turn_rate; look: the look control's pitch state, max speed +24 over drag +28, as its
// integrator settles). The game's drag then spreads the turn over a few frames and keeps its total.
// Faster than the stick turns, the command stays at full and the rest waits for the next frames
// (g_stick_pending): the AC turns at its top speed until it has turned what the mouse asked, up to
// a half turn, so a quick flick turns the whole way; the look waits only for what its limits leave.
// The axes are the ones the game's key type gives turning and looking (TYPE A / B: the right stick).
// The game's vertical inversion option inverts the stick's look; the mouse's look is turned
// against it, so [mouse] invert_y alone decides it. The stick's own deflection (keys bound to it, a
// controller) adds its command.
constexpr u32 kPadPort = 384, kPadSticks = 1172;  // pad class 0x951f94: the port; the stick floats
constexpr double kFallbackFullTurn = 0.02;        // radians per step, before the probe knows the AC's (about 70 deg/s)
constexpr double kFallbackFullPitch = 0.02;
constexpr int kProbeFrames = 30;                  // the turn rate probe runs while the mouse moved this recently
// A turn this much faster than the stick's own top speed is not the stick's (the game set the
// heading): what waits is dropped. The AC turns once per game frame, and pad reads can come closer
// together than frames (a turn of 2.2 degrees, one frame's at full speed, 8.5 ms after the previous
// read; run 20261029-142639): the measure is at least one frame's turn, and at least kFastTurnMin.
constexpr double kFastTurn = 2.5;
constexpr double kFastTurnMin = 0.1;  // radians (5.7 degrees)

LARGE_INTEGER g_stick_last{};        // the previous port 0 pad update
int g_stick_recent = 0;              // frames left in which the turn rate probe runs
double g_stick_pending[2] = {0, 0};  // the mouse's turn (right +) and look (up +) the stick has not given yet (radians)
double g_stick_assumed[2] = {0, 0};  // what the deflection written at the previous read gives, if the game uses it
u64 g_stick_frame = 0;               // the operator's frame count at the previous read (g_turn_calls)
double g_stick_heading = 0;          // the player AC's heading at the previous pad update
u32 g_stick_body = 0;                // and its body
// The game's frame interval (the player's operator, once per frame): a deflection acts over a
// frame's 1/60 s steps (the turn step adds its force per step), whatever the pad reads' spacing.
std::atomic<double> g_frame_steps{0};
LARGE_INTEGER g_operator_last{};

// The pad float an action's input code moves (-1: a button or none) and the sign of the action's
// direction on it (floats: right X, right Y up +, left X, left Y up +).
int float_of_code(u8 code, int* sign) {
  switch (code) {
    case 0x06: *sign = 1; return 0;   // right stick right
    case 0x07: *sign = -1; return 0;  // right stick left
    case 0x04: *sign = 1; return 1;   // right stick up
    case 0x05: *sign = -1; return 1;  // right stick down
    case 0x02: *sign = 1; return 2;   // left stick right
    case 0x03: *sign = -1; return 2;  // left stick left
    case 0x00: *sign = 1; return 3;   // left stick up
    case 0x01: *sign = -1; return 3;  // left stick down
    default: *sign = 0; return -1;
  }
}
// The float an axis pair of actions (turn right / left, look up / down) sits on, with the sign of
// the first action; -1 unless both are the two ends of one stick axis.
int axis_of(u32 plus_slot, u32 minus_slot, int* sign) {
  u8 codes[rt::game::kSlotCount];
  if (!keys(codes)) {  // TYPE A until the game's assignment is known
    *sign = 1;
    return plus_slot == rt::game::kSlotTurnRight ? 0 : 1;
  }
  int sp = 0, sm = 0;
  int fp = float_of_code(codes[plus_slot], &sp), fm = float_of_code(codes[minus_slot], &sm);
  if (fp < 0 || fp != fm || sp != -sm) return -1;
  *sign = sp;
  return fp;
}

// The command that gives an angle over the frame (radians; rate: what a command of 1 turns per
// 1/60 s step, steps: the frame's), at most cmax, which turns rmax per step: what is left waits.
struct AxisCommand {
  double command = 0, rest = 0;
};
AxisCommand axis_command(double want, double rate, double steps, double cmax, double rmax) {
  AxisCommand a;
  double c = want / (rate * steps);
  if (std::fabs(c) < cmax) {
    a.command = c;
    return a;
  }
  a.command = std::copysign(cmax, want);
  double rest = want - std::copysign(rmax * steps, want);
  a.rest = rest * want > 0 ? rest : 0;  // a full command may turn a little more than asked: then nothing waits
  return a;
}

// After each port 0 pad update: the frame's mouse movement onto the stick.
void mouse_stick_frame(u32 pad) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_stick_last.QuadPart ? double(now.QuadPart - g_stick_last.QuadPart) / double(freq.QuadPart) : 0;
  g_stick_last = now;
  if (g_stick_recent > 0) --g_stick_recent;
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  if (m.mode != rt::bindings::MouseMode::Stick) {  // camera mode takes the mouse in the operator
    g_stick_pending[0] = g_stick_pending[1] = 0;
    return;
  }
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  // A second read before the game's next frame replaces the deflection the frame has not used yet:
  // its share waits again.
  u64 frame = g_turn_calls.load(std::memory_order_relaxed);
  if (frame == g_stick_frame) {
    g_stick_pending[0] += g_stick_assumed[0];
    g_stick_pending[1] += g_stick_assumed[1];
  }
  g_stick_frame = frame;
  g_stick_assumed[0] = g_stick_assumed[1] = 0;
  // The AC turned faster than the stick turns it (the game set its heading): the turn that waited is done.
  u32 body = player_body();
  double heading = 0;
  bool has_heading = heading_of(body, &heading);
  double top = std::max({g_full[0], g_full[1], kFallbackFullTurn});
  if (has_heading && body == g_stick_body && g_stick_pending[0] != 0 && dt > 0 &&
      std::fabs(wrap(heading - g_stick_heading)) > std::max(kFastTurn * top * std::max(dt * 60, 1.0), kFastTurnMin)) {
    static int logged = 0;
    if (logged++ < 20)
      LOG_I("input", "mouse stick: the AC turned %.1f deg in %.1f ms (not the stick's turn): %.1f deg left to turn dropped",
            wrap(heading - g_stick_heading) * 180 / kPi, dt * 1000, g_stick_pending[0] * 180 / kPi);
    g_stick_pending[0] = 0;
  }
  g_stick_body = has_heading ? body : 0;
  g_stick_heading = heading;
  if (!dx && !dy && g_stick_pending[0] == 0 && g_stick_pending[1] == 0) return;
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  bool gameplay = rt::game::input_context() == rt::game::InputContext::Gameplay;
  if (dt <= 0 || dt > 0.25 || !gameplay || !g_curve.known || !rt::vm::committed(pad + kPadSticks, 16)) {
    if ((dx || dy) && traced++ < 40)
      LOG_I("input", "mouse stick: %lld, %lld counts dropped (%.0f ms since the previous frame%s%s)", (long long)dx, (long long)dy, dt * 1000,
            gameplay ? "" : ", not in gameplay", g_curve.known ? "" : ", stick response not known yet");
    g_stick_pending[0] = g_stick_pending[1] = 0;  // after a pause or a menu nothing from before turns the AC
    return;
  }
  g_stick_recent = kProbeFrames;
  double steps = g_frame_steps.load(std::memory_order_relaxed);
  if (!(steps > 0)) steps = dt * 60;
  double mx = double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;                   // right +
  double my = double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;  // up +
  double t = now_seconds();
  if (mx != 0) {  // the tutorial's steps
    g_activity.turned_at = t;
    g_activity.turn_dir = mx > 0 ? 1 : -1;
  }
  if (my != 0) g_activity.pitched_at = t;
  double yaw = std::clamp(g_stick_pending[0] + mx, -kPi, kPi);  // at most a half turn waits
  double pitch = g_stick_pending[1] + my;
  u32 look = g_look.load(std::memory_order_relaxed);
  if (look && rt::vm::committed(look + kPitchState, 40)) {  // the look waits only for what its limits leave
    double angle = rf(look + kPitchState), hi = rf(look + kPitchState + 32), lo = rf(look + kPitchState + 36);
    if (lo < hi) pitch = std::clamp(pitch, std::min(0.0, lo - angle), std::max(0.0, hi - angle));
  }
  // The turn step takes an AC's turn speed for a direction only at a full command (+1 or -1) and
  // the mean of both otherwise (0xa10260): below full the command scales the mean. Where a full
  // command turns slower than the mean (one side of most ACs), the mouse's command stops just short
  // of full, so moving faster never turns slower.
  bool rates = g_full[0] > 0 && g_full[1] > 0;
  double partial = rates ? (g_full[0] + g_full[1]) / 2 : kFallbackFullTurn;
  double full_pitch = pitch_full_rate();
  if (full_pitch <= 0) full_pitch = kFallbackFullPitch;
  int side = yaw < 0 ? 0 : 1;
  bool slow = rates && g_full[side] < partial * kNearFull;
  AxisCommand turn = axis_command(yaw, partial, steps, slow ? kNearFull : 1.0, slow ? partial * kNearFull : rates ? g_full[side] : partial);
  AxisCommand lift = axis_command(pitch, full_pitch, steps, 1.0, full_pitch);
  g_stick_pending[0] = turn.rest;
  g_stick_pending[1] = lift.rest;
  g_stick_assumed[0] = yaw - turn.rest;
  g_stick_assumed[1] = pitch - lift.rest;
  double commands[2] = {turn.command, g_look_inverted ? -lift.command : lift.command};
  std::string out;
  for (int a = 0; a < 2; ++a) {
    if (commands[a] == 0) continue;
    int sign = 0;
    int f = a == 0 ? axis_of(rt::game::kSlotTurnRight, rt::game::kSlotTurnLeft, &sign) : axis_of(rt::game::kSlotLookUp, rt::game::kSlotLookDown, &sign);
    if (f < 0) {  // the key type puts this action on buttons or split across sticks: the mouse cannot press it
      static int logged = 0;
      if (logged++ < 4) LOG_I("input", "mouse stick: the game's assignment has no stick axis for %s", a == 0 ? "turning" : "looking");
      g_stick_pending[a] = g_stick_assumed[a] = 0;
      continue;
    }
    u32 ea = pad + kPadSticks + 4 * u32(f);
    double base = sign * g_curve.value(rf(ea));  // the stick's own command in the action's direction
    double total = std::clamp(base + commands[a], -1.0, 1.0);
    wf(ea, float(sign * g_curve.deflection(total)));
    if (traced < 40) out += ac4::strprintf(" %s %.4f -> float %u %.4f;", a == 0 ? "turn" : "look", total, u32(f), double(rf(ea)));
  }
  if (traced++ < 40)
    LOG_I("input", "mouse stick: %lld, %lld counts over %.1f ms (rates %.5f, %.5f rad per step; %.4f, %.4f rad wait):%s", (long long)dx, (long long)dy,
          dt * 1000, partial, full_pitch, turn.rest, lift.rest, out.c_str());
}

// ---- The mouse camera (mouse mode "camera") ----
// The mouse moves the camera (user decision 2026-10-16): raw counts become angles (kRadPerCount per
// count times the sensitivity; no smoothing, acceleration or dead zone) that turn and pitch the
// camera at once, however fast the mouse moves. The AC then turns toward where the camera looks by
// the game's own turning: the turn command it gets is the stick's, fully pushed while the camera is
// more than kChaseFull away and proportionally less within, so it turns at its own speed and stops
// where the camera points.
//   Only the camera turns freely (as the ACFA port's 0.7.3: lock-on, firing and movement stay with
// the AC's front). Everything that looks or aims from the AC takes its view matrix (0x9ac8d4: the
// body times the look control's rotation, whose yaw block look +4 the game holds at 0); the game
// camera is the one caller at 0x9f2444 (in 0x9f21b8; run 20261029-145619: that caller's matrix
// turned 45 degrees swung the camera around the AC, the five others moved nothing on screen). The
// mouse camera turns that matrix alone about the vertical to its heading (hook_view_matrix), so the
// game still places the camera (distance, framing, walls) and the FCS, the radar and the movement
// keep the AC's front. Until 0.5.x the mouse camera wrote look +4, which also turned the FCS.
//   Other turning stays the game's: the stick or turn keys turn the AC with the camera at its
// angle; once the AC points where the camera does and the mouse rests, the camera rides with the
// AC again and follows whatever turns it (auto-sighting, scripts). Where the view takes no turn
// (scripted moments) or during a view reset the camera goes back behind the AC.
//   Pitch: the look control's pitch angle (+48) gets the mouse's angle after its update, within
// its limits +80 (up) / +84 (down), as the stick's look does (the FCS looks up and down with it);
// mode +68 is 1 while a view reset recentres it.
constexpr u32 kCameraViewReturn = 0x009f2444;  // the game camera's call of the view matrix (0x9f21b8)
constexpr double kChaseFull = 8.0 * kPi / 180;  // the camera further than this from the AC's heading: full turn command
constexpr double kSettled = 0.2 * kPi / 180;    // nearer, with the mouse at rest: the AC points where the camera does
constexpr double kStickTurn = 0.05;             // a turn command from the stick or the turn keys

// The camera's state (the game's main thread: the operator, the look control and the AC update all
// run there, in that order).
struct MouseCam {
  u32 body = 0;            // the player's AC body the headings belong to
  double target = 0;       // where the camera looks: a heading in the body's convention (radians)
  double heading = 0;      // the body's heading at this frame's start
  double offset = 0;       // target - heading: the camera's yaw relative to the AC this frame (0: it rides with the AC)
  double pitch = 0;        // this frame's mouse pitch (radians, up positive)
  double command = 0;      // the turn command the AC got this frame (dev trace)
  bool fresh = false;      // set by the operator's turn call; the look update applies the frame's pitch
  bool free = false;       // the camera looks at target (else it rides with the AC); read by the view matrix hook
  double at = -1;          // when the operator last ran (seconds)
};
MouseCam g_cam;
LARGE_INTEGER g_last_frame{};

// Once per game frame, from the operator's turn call (before the pitch call, the look control and
// the AC's update; f1 the turn command from the stick and the turn keys): the mouse's movement
// since the previous frame turns the camera, and the AC gets the command that turns it toward the
// camera.
void mouse_cam_frame(Ctx& ctx, u32 view) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) / double(freq.QuadPart) : 0;
  g_last_frame = now;
  const rt::bindings::MouseSettings m = rt::bindings::mouse();
  if (m.mode != rt::bindings::MouseMode::Camera) {  // the stick mode takes the mouse at the pad (mouse_stick_frame)
    g_cam = MouseCam{};
    return;
  }
  s64 dx = 0, dy = 0;
  rt::input_mouse_take(&dx, &dy);
  u32 body = player_body();
  double h = 0;
  if (!heading_of(body, &h)) {
    g_cam = MouseCam{};
    return;
  }
  if (body != g_cam.body) {  // a new AC (a mission, AC TEST, the tutorial's next part): the camera behind it
    g_cam = MouseCam{};
    g_cam.body = body;
    g_cam.target = g_cam.heading = h;
  }
  double turned_by = wrap(h - g_cam.heading);  // the AC's turn since the previous frame
  g_cam.heading = h;
  // Menus, a pause, a hitch: what the mouse did meanwhile moves nothing.
  bool gap = dt <= 0 || dt > 0.25;
  double t = gap ? 0 : double(m.invert_x ? -dx : dx) * m.sensitivity_x * kRadPerCount;
  double p = gap ? 0 : double(m.invert_y ? -dy : dy) * m.sensitivity_y * kRadPerCount * kPitchDownSign;
  static int traced = 0;  // diagnostics: the first frames with mouse movement
  if ((dx || dy) && traced++ < 40)
    LOG_I("input", "mouse camera: %lld, %lld counts over %.1f ms%s", (long long)dx, (long long)dy, dt * 1000, gap ? " (dropped)" : "");
  u32 look = g_look.load(std::memory_order_relaxed);
  bool recentring = look && rt::vm::committed(look + kPitchState + 20, 4) && r32(look + kPitchState + 20) == 1;
  double stick = ctx.f[1];
  bool stick_turns = std::fabs(stick) > kStickTurn;
  // A heading change faster than an AC turns (about 0.03 rad per 1/60 s step at full speed; twice
  // that allowed) is the game setting it (a start position, a scripted turn), as is any change
  // over a gap.
  double steps = std::min(dt, 0.25) * 60;
  bool set_by_game = gap || std::fabs(turned_by) > std::max(0.07 * steps, 0.05);
  if (!view_allows(view, 2) || recentring) {
    g_cam.target = h;  // no turning here (or a view reset): the camera behind the AC
    t = 0;
  } else if (stick_turns || set_by_game) {
    g_cam.target += turned_by;  // the stick turns the AC, or the game set its heading: the camera keeps its angle to it
  }
  g_cam.target = wrap(g_cam.target + t);
  double o = wrap(g_cam.target - h);
  if (t == 0 && std::fabs(o) < kSettled) {  // caught up and the mouse at rest: the camera follows the AC again
    g_cam.target = h;
    o = 0;
  }
  g_cam.offset = o;
  g_cam.free = o != 0;
  g_cam.command = stick;
  bool chase = o != 0 && !stick_turns;
  if (chase) {  // the AC turns toward the camera as with the stick
    g_cam.command = std::clamp(o / kChaseFull, -1.0, 1.0);
    ctx.f[1] = g_cam.command;
  }
  g_cam.pitch = view_allows(view, 4) && !recentring ? p : 0;
  double at = double(now.QuadPart) / double(freq.QuadPart);
  if (t != 0) {
    g_activity.turned_at = at;
    g_activity.turn_dir = t > 0 ? 1 : -1;
  }
  if (p != 0 && view_allows(view, 4)) g_activity.pitched_at = at;
  g_cam.at = at;
  g_cam.fresh = true;
}

// After the player's look control updated: the mouse's pitch within the limits. Nothing when the
// operator did not run this frame (cutscenes, menus).
void mouse_cam_apply(u32 look) {
  if (!g_cam.fresh) return;
  g_cam.fresh = false;
  u32 p = look + kPitchState;
  if (g_cam.pitch != 0 && rt::vm::committed(p, 40)) {
    double hi = rf(p + 32), lo = rf(p + 36);
    if (lo < hi) wf(p, float(std::clamp(double(rf(p)) + g_cam.pitch, lo, hi)));
  }
  g_cam.pitch = 0;
}

// The operator's turn call of the player (any of the three input classes): its action table, the
// stick's response, the player's input state, then the mouse camera's frame.
void player_turn_call(Ctx& ctx) {
  g_turn_calls.fetch_add(1, std::memory_order_relaxed);
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dt = g_operator_last.QuadPart ? double(now.QuadPart - g_operator_last.QuadPart) / double(freq.QuadPart) : 0;
  g_operator_last = now;
  g_frame_steps.store(dt > 0 && dt < 0.25 ? dt * 60 : 0, std::memory_order_relaxed);
  note_key_table(ctx);
  read_stick_settings(ctx);
  u32 view = u32(ctx.r[3]);
  if (view && rt::vm::committed(view + 12, 4)) {
    u32 state = r32(view + 12);
    if (g_player_state.exchange(state) != state) {
      LOG_I("input", "player input 0x%08x (vtable %08x), state 0x%08x, input record 0x%08x", view, r32(view), state, u32(ctx.r[28]));
      rt::datawatch::set_base("state", state);  // --watch-access state+OFFSET (turn command +32, pitch +48)
    }
  }
  mouse_cam_frame(ctx, view);
}

// 0x9fc210 (every frame) and 0x9fc364 (while there is turn input): r3 an AC's controller, r4 its
// input. The player's (its input holds the player's state) gives the player's turn object,
// r32(r32(r32(controller +4) +152) +260); its +4 is the AC body.
u32 note_turn_input(Ctx& ctx) {
  u32 ctl = u32(ctx.r[3]), input = u32(ctx.r[4]);
  u32 state = g_player_state.load(std::memory_order_relaxed);
  if (!state || !input || !rt::vm::committed(input + 12, 4) || r32(input + 12) != state || !rt::vm::committed(ctl + 4, 4)) return 0;
  u32 a = r32(ctl + 4);
  if (!a || !rt::vm::committed(a + 152, 4)) return 0;
  u32 b = r32(a + 152);
  if (!b || !rt::vm::committed(b + 260, 4)) return 0;
  u32 obj = r32(b + 260);
  if (g_player_turn.exchange(obj) != obj) {
    LOG_I("input", "mouse: player turn object 0x%08x", obj);
    g_full[0] = g_full[1] = 0;  // another AC: its own turn rate
  }
  return obj;
}

}  // namespace

// Pad object updates, once per frame and port: cellPadGetData(port, this + port offset + 8), then
// the sticks normalized to floats in [-1, 1] ((v / 255 - 0.5) * 2, Y inverted); r3 1 when it read
// the pad. Two classes with the same layout at different offsets (0x951f94: port +384, sticks
// +1172; 0x5ead28: port +736, sticks +1524); port 0 uses the first: the mouse as the right stick.
void hook_pad_update(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 pad = u32(ctx.r[3]);
  orig(ctx);
  if (u32(ctx.r[3]) == 1 && rt::vm::committed(pad + kPadPort, 4) && r32(pad + kPadPort) == 0) mouse_stick_frame(pad);
}

void hook_pad_update_b(Ctx& ctx, void (*orig)(Ctx&)) {
  orig(ctx);
}

// View slot 6 (0xb6b148: r3 view, f1 turn command): from the player's operator, the frame's mouse,
// which may replace the command with the one that turns the AC toward the mouse camera.
void hook_view_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) player_turn_call(ctx);
  orig(ctx);
}

// The tutorial's view class (vtable 0x10148768) has its own slot 6 (0xb6acf4): the same store, plus
// a flag its steps count ("turn 3 times") when the command is not zero. The AC turning counts as the
// stick does; after a mouse turn the AC may stop sooner than the step needs, so the turn input goes
// on (kTutorialTurn) until kTutorialHold after the mouse rested.
void hook_view_turn_set_b(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) {
    player_turn_call(ctx);
    if (std::fabs(ctx.f[1]) < kTutorialTurn && g_activity.turning()) ctx.f[1] = g_activity.turn_dir * kTutorialTurn;
  }
  orig(ctx);
}

// The plain input class's slot 6 (0xb67cc8: r3 input, f1 turn command; a store at its state +32):
// the player's in the SIMULATOR (the operator's call), AI inputs elsewhere.
void hook_input_turn_set(Ctx& ctx, void (*orig)(Ctx&)) {
  if (u32(ctx.lr) == kPlayerTurnSetReturn) {
    player_turn_call(ctx);
  }
  orig(ctx);
}

// View slot 9 (0xb67970, a thunk to slot 8: r3 view, f1 pitch command, r5 the operator's "pitch
// input" flag): the stick's command goes on unchanged. The tutorial view's slot 8 (0xb6ae5c) marks
// its "adjust view" input on that flag (r32(view +24) +72; slot 8 of the other views ignores it): a
// mouse look raises it from its start until kTutorialHold after the mouse rested.
void hook_view_pitch_set(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 lr = u32(ctx.lr);
  if (lr == kPlayerPitchSetReturns[0] || lr == kPlayerPitchSetReturns[1]) {
    g_pitch_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_activity.pitching()) ctx.r[5] = 1;
  }
  orig(ctx);
}

// The turn inputs (0x9fc210, 0x9fc364): the player's turn object; after the frame's turn input, for
// the player's while the mouse moves the stick (and once before): the stick's full turning speed
// each way (the scale of the mouse stick; kept in between).
void hook_ac_turn_input(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 obj = note_turn_input(ctx);
  orig(ctx);
  bool stick = rt::bindings::mouse().mode == rt::bindings::MouseMode::Stick && (g_stick_recent > 0 || g_full[1] <= 0);
  if (!obj || !stick) return;
  if (double r = full_turn_rate(ctx, obj, 1); r > 0) g_full[1] = r;  // the probe restores ctx
  if (double l = full_turn_rate(ctx, obj, -1); l > 0) g_full[0] = l;
}
void hook_ac_turn_input_b(Ctx& ctx, void (*orig)(Ctx&)) {
  note_turn_input(ctx);
  orig(ctx);
}

namespace {
// Turns a view matrix (4x4 floats, rows 0-2 the axes, row 3 the position) about the world's
// vertical (+y) by a radians at its position: its heading (atan2(x, z) of row 2, as an AC body's)
// grows by a, a right turn.
void rotate_view(u32 m, double a) {
  double c = std::cos(a), s = std::sin(a);
  for (u32 row = 0; row < 3; ++row) {
    u32 r = m + 16 * row;
    double x = rf(r), z = rf(r + 8);
    wf(r, float(x * c + z * s));
    wf(r + 8, float(z * c - x * s));
  }
}
double view_heading(u32 m) { return std::atan2(double(rf(m + 32)), double(rf(m + 40))); }
}  // namespace

// An AC's view matrix (0x9ac8d4: r3 the 4x4 result, r4 the AC; r32(AC +240) slot 6 gives the look
// control's rotation, r32(AC +76) slot 15 the body's transform, 0x9a8b60 their product, row 3 the
// head's position): everything that looks or aims from the AC takes it (runs 20261029-143341:
// the readers of the look control's yaw and pitch are all its callers).
void hook_view_matrix(Ctx& ctx, void (*orig)(Ctx&)) {
  u32 out = u32(ctx.r[3]), ac = u32(ctx.r[4]), lr = u32(ctx.lr);
  orig(ctx);
  // The mouse camera: the game camera's view turned to the camera's heading (its operator frame
  // recent: nothing in cutscenes and menus, where the operator does not run).
  if (lr == kCameraViewReturn && g_cam.free && now_seconds() - g_cam.at < 0.25 && ac && rt::vm::committed(ac + 240, 4) &&
      r32(ac + 240) == g_look.load(std::memory_order_relaxed) && rt::vm::committed(out, 64))
    rotate_view(out, wrap(g_cam.target - view_heading(out)));
}

// The AC's look control (0xa372e8: r3 control, r4 input, f1 frame time). For the player's (its
// input holds the player's state at +12): the mouse camera after the update.
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
    LOG_I("input", "player look control 0x%08x (input 0x%08x), camera yaw at 0x%08x, pitch at 0x%08x", look, input, look + kLookYaw,
          look + kPitchState);
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
  mouse_cam_apply(look);
}

}  // namespace ppu

namespace rt::game {
bool key_assignment(u8 codes[kSlotCount]) { return ppu::keys(codes); }
}  // namespace rt::game

